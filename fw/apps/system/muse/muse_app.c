/* SPDX-License-Identifier: Apache-2.0 */

#include "muse_app.h"

#ifdef CONFIG_SERVICE_AUDIO_COMPANION_PTT

#include <applib/app.h>
#include <applib/app_launch_reason.h>
#include <applib/app_timer.h>
#include <applib/fonts/fonts.h>
#include <applib/graphics/graphics.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/kino/kino_layer.h>
#include <applib/ui/scroll_layer.h>
#include <applib/ui/text_layer.h>
#include <applib/ui/vibes.h>
#include <applib/ui/window.h>
#include <kernel/pbl_malloc.h>
#include <pbl/services/audio_companion.h>
#include <pbl/services/light.h>
#include <process_state/app_state/app_state.h>
#include <resource/resource_ids.auto.h>

#include <string.h>

//! The progress bar moves while listening; everything else changes at the phone's pace.
#define MUSE_POLL_LISTENING_MS (100)
#define MUSE_POLL_MS (250)
//! Holding Select starts a question after the same time a Quick Launch hold takes.
#define MUSE_HOLD_MS (400)
#define MUSE_MARGIN (10)
#define MUSE_GAP (4)
#define MUSE_BAR_HEIGHT (6)

#define MUSE_ACCENT PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack)
#define MUSE_SECONDARY PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack)

typedef struct {
  uint32_t icon;
  bool accent;  // the listening screen: the mic is on
  bool centered;
  const char *title;
  const char *line;
  const char *heard;
  const char *answer;
  const char *footer;
} MuseScreen;

typedef struct {
  Window *window;
  ScrollLayer *scroll;
  KinoLayer *icon;
  TextLayer *title;
  TextLayer *line;
  TextLayer *heard;
  TextLayer *answer;
  TextLayer *footer;
  Layer *bar;
  AppTimer *timer;

  AudioCompanionAssistantView view;
  uint32_t generation;
  bool have_view;
  bool available;
  //! A question was asked from this screen (or brought it up): the next one is a follow-up.
  bool asked;
  uint8_t answered_turn;
  uint8_t max_turn;
  uint32_t shown_icon;
} MuseAppData;

// ---- what to show ----

static const char *prv_error_title(uint8_t error) {
  switch (error) {
    case AudioCompanionAssistantErrorNotSetUp: return "Muse isn't set up";
    case AudioCompanionAssistantErrorUnreachable: return "Can't reach Muse";
    case AudioCompanionAssistantErrorNoAnswer: return "No answer";
    case AudioCompanionAssistantErrorDidNotCatch: return "Didn't catch that";
    case AudioCompanionAssistantErrorBusy: return "Muse is busy";
    case AUDIO_COMPANION_ASSISTANT_ERROR_MIC_UNAVAILABLE: return "Microphone busy";
    case AUDIO_COMPANION_ASSISTANT_ERROR_PHONE_UNAVAILABLE:
    default: return "Can't reach your phone";
  }
}

static const char *prv_error_line(uint8_t error) {
  switch (error) {
    case AudioCompanionAssistantErrorNotSetUp: return "Set it up in Pebble Audio on your phone.";
    case AudioCompanionAssistantErrorUnreachable: return "Check your phone's connection.";
    case AudioCompanionAssistantErrorNoAnswer: return "Muse didn't reply. Try again.";
    case AudioCompanionAssistantErrorDidNotCatch: return "Hold Select and try again.";
    case AudioCompanionAssistantErrorBusy:
    case AUDIO_COMPANION_ASSISTANT_ERROR_MIC_UNAVAILABLE: return "Try again in a moment.";
    case AUDIO_COMPANION_ASSISTANT_ERROR_PHONE_UNAVAILABLE:
    default: return "Muse works through Pebble Audio on your phone.";
  }
}

static MuseScreen prv_screen(const MuseAppData *data) {
  const AudioCompanionAssistantView *v = &data->view;
  const char *heard = v->heard[0] ? v->heard : NULL;
  switch (v->phase) {
    case AudioCompanionAssistantPhaseListening:
      return (MuseScreen){
        .icon = RESOURCE_ID_VOICE_MICROPHONE_LARGE,
        .accent = true,
        .centered = true,
        .title = "Listening",
        .line = "Release to send",
      };
    case AudioCompanionAssistantPhaseSending:
      return (MuseScreen){
        .centered = true,
        .title = "Sending",
        .line = v->reached_max ? "Questions can run 15 seconds." : NULL,
      };
    case AudioCompanionAssistantPhaseWaiting:
      return (MuseScreen){
        .centered = !heard,
        .title = v->status[0] ? v->status : "Thinking\xE2\x80\xA6",
        .heard = heard,
      };
    case AudioCompanionAssistantPhaseAnswering:
    case AudioCompanionAssistantPhaseDone:
      return (MuseScreen){
        .heard = heard,
        .answer = v->answer[0] ? v->answer : "Done.",
        .footer = v->phase == AudioCompanionAssistantPhaseDone ? "Hold Select to ask again"
                                                                : NULL,
      };
    case AudioCompanionAssistantPhaseError:
      return (MuseScreen){
        .icon = RESOURCE_ID_GENERIC_WARNING_LARGE,
        .centered = true,
        .title = prv_error_title(v->error),
        .line = prv_error_line(v->error),
        .heard = v->error == AudioCompanionAssistantErrorDidNotCatch ? NULL : heard,
      };
    case AudioCompanionAssistantPhaseIdle:
    default:
      break;
  }
  if (!data->available) {
    return (MuseScreen){
      .icon = RESOURCE_ID_GENERIC_WARNING_LARGE,
      .centered = true,
      .title = prv_error_title(AUDIO_COMPANION_ASSISTANT_ERROR_PHONE_UNAVAILABLE),
      .line = prv_error_line(AUDIO_COMPANION_ASSISTANT_ERROR_PHONE_UNAVAILABLE),
    };
  }
  return (MuseScreen){
    .icon = RESOURCE_ID_VOICE_MICROPHONE_LARGE,
    .centered = true,
    .title = "Ask Muse",
    .line = "Hold Select and speak",
  };
}

// ---- layout ----

//! Places `layer` at y with `text`, or hides it. Returns the y below it.
static int16_t prv_place_text(TextLayer *layer, const char *text, int16_t y, int16_t width) {
  Layer *l = text_layer_get_layer(layer);
  if (!text) {
    layer_set_hidden(l, true);
    return y;
  }
  layer_set_hidden(l, false);
  text_layer_set_text(layer, text);
  layer_set_frame(l, &GRect(MUSE_MARGIN, y, width, 2000));
  const GSize size = text_layer_get_content_size(app_state_get_graphics_context(), layer);
  const int16_t height = size.h + MUSE_GAP;
  layer_set_frame(l, &GRect(MUSE_MARGIN, y, width, height));
  return y + height + MUSE_GAP;
}

static void prv_set_colors(MuseAppData *data, bool accent) {
  const GColor text = accent ? GColorWhite : GColorBlack;
  const GColor secondary = accent ? GColorWhite : MUSE_SECONDARY;
  window_set_background_color(data->window, accent ? MUSE_ACCENT : GColorWhite);
  text_layer_set_text_color(data->title, text);
  text_layer_set_text_color(data->line, text);
  text_layer_set_text_color(data->answer, text);
  text_layer_set_text_color(data->heard, secondary);
  text_layer_set_text_color(data->footer, secondary);
}

static void prv_layout(MuseAppData *data, bool scroll_to_top) {
  const MuseScreen screen = prv_screen(data);
  const GRect bounds = layer_get_bounds_by_value(window_get_root_layer(data->window));
  const int16_t width = bounds.size.w - 2 * MUSE_MARGIN;
  prv_set_colors(data, screen.accent);

  const GTextAlignment align = screen.centered ? GTextAlignmentCenter : GTextAlignmentLeft;
  text_layer_set_text_alignment(data->title, align);
  text_layer_set_text_alignment(data->line, align);
  text_layer_set_text_alignment(data->heard, align);

  int16_t y = MUSE_MARGIN;
  Layer *icon = kino_layer_get_layer(data->icon);
  if (screen.icon) {
    if (screen.icon != data->shown_icon) {
      kino_layer_set_reel_with_resource(data->icon, screen.icon);
      data->shown_icon = screen.icon;
    }
    const GSize size = kino_reel_get_size(kino_layer_get_reel(data->icon));
    layer_set_frame(icon, &GRect((bounds.size.w - size.w) / 2, y, size.w, size.h));
    layer_set_hidden(icon, false);
    y += size.h + MUSE_GAP * 2;
  } else {
    layer_set_hidden(icon, true);
  }
  y = prv_place_text(data->title, screen.title, y, width);
  y = prv_place_text(data->line, screen.line, y, width);
  y = prv_place_text(data->heard, screen.heard, y, width);
  y = prv_place_text(data->answer, screen.answer, y, width);
  y = prv_place_text(data->footer, screen.footer, y, width);
  y += MUSE_MARGIN;

  // Short screens sit in the middle; long ones scroll.
  const int16_t visible = bounds.size.h - (screen.accent ? MUSE_BAR_HEIGHT + MUSE_MARGIN : 0);
  Layer *scroll = scroll_layer_get_layer(data->scroll);
  if (screen.centered && y < visible) {
    layer_set_frame(scroll, &GRect(0, (visible - y) / 2, bounds.size.w, y));
  } else {
    layer_set_frame(scroll, &GRect(0, 0, bounds.size.w, bounds.size.h));
  }
  scroll_layer_set_content_size(data->scroll, GSize(bounds.size.w, y));
  if (scroll_to_top) {
    scroll_layer_set_content_offset(data->scroll, GPointZero, false);
  }
  layer_set_hidden(data->bar, !screen.accent);
  layer_mark_dirty(data->bar);
}

static void prv_bar_update_proc(Layer *layer, GContext *ctx) {
  MuseAppData *data = app_state_get_user_data();
  const GRect bounds = layer_get_bounds_by_value(layer);
  const uint32_t limit_ds = data->view.max_seconds ? data->view.max_seconds * 10u : 150u;
  const uint32_t done = MIN(data->view.recorded_ds, limit_ds);
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorDarkGray));
  graphics_fill_round_rect(ctx, &bounds, MUSE_BAR_HEIGHT / 2, GCornersAll);
  GRect fill = bounds;
  fill.size.w = (int16_t)((bounds.size.w * done) / limit_ds);
  if (fill.size.w > 0) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_round_rect(ctx, &fill, MUSE_BAR_HEIGHT / 2, GCornersAll);
  }
}

// ---- following the question ----

static void prv_schedule(MuseAppData *data);

static void prv_refresh(MuseAppData *data) {
  const bool was_available = data->available;
  data->available = audio_companion_ptt_available();
  const AudioCompanionAssistantPhase was_phase = data->view.phase;
  const uint8_t was_turn = data->view.turn;
  const bool was_max = data->view.reached_max;
  const uint32_t generation = audio_companion_assistant_get_view(NULL);
  if (data->have_view && generation == data->generation && was_available == data->available) {
    return;
  }
  audio_companion_assistant_get_view(&data->view);
  data->generation = generation;
  const bool first = !data->have_view;
  data->have_view = true;
  const AudioCompanionAssistantView *v = &data->view;
  if (v->phase != AudioCompanionAssistantPhaseIdle && v->turn > data->max_turn) {
    data->max_turn = v->turn;
    data->asked = true;
  }

  const bool answer_phase = v->phase == AudioCompanionAssistantPhaseAnswering ||
                            v->phase == AudioCompanionAssistantPhaseDone;
  if (!first && answer_phase && data->answered_turn != v->turn) {
    // The first words of an answer: a tap on the wrist, and the light, so it can be read.
    data->answered_turn = v->turn;
    vibes_short_pulse();
    light_enable_interaction();
  } else if (!first && v->reached_max && !was_max && v->turn == was_turn) {
    vibes_double_pulse();
  }
  // A new screen starts at the top; an answer that keeps arriving stays where it was read.
  const bool new_screen = first || v->turn != was_turn || v->phase != was_phase;
  const bool still_answering = answer_phase && was_phase == AudioCompanionAssistantPhaseAnswering;
  prv_layout(data, new_screen && !still_answering);
}

static void prv_timer_cb(void *context) {
  MuseAppData *data = context;
  data->timer = NULL;
  prv_refresh(data);
  prv_schedule(data);
}

static void prv_schedule(MuseAppData *data) {
  const uint32_t ms = data->view.phase == AudioCompanionAssistantPhaseListening
                          ? MUSE_POLL_LISTENING_MS : MUSE_POLL_MS;
  data->timer = app_timer_register(ms, prv_timer_cb, data);
}

// ---- buttons ----

static void prv_select_down(ClickRecognizerRef recognizer, void *context) {
  MuseAppData *data = app_state_get_user_data();
  // Listening starts at the press so the first words are kept; it only becomes a question once
  // the hold lasts (prv_select_hold). The release is handled by the service, wherever it lands.
  audio_companion_ptt_arm(BUTTON_ID_SELECT, data->asked ? AudioCompanionPttTriggerFollowUp
                                                        : AudioCompanionPttTriggerInApp);
}

static void prv_select_hold(ClickRecognizerRef recognizer, void *context) {
  MuseAppData *data = app_state_get_user_data();
  audio_companion_ptt_commit();
  prv_refresh(data);
}

static void prv_click_config(void *context) {
  window_raw_click_subscribe(BUTTON_ID_SELECT, prv_select_down, NULL, NULL);
  window_long_click_subscribe(BUTTON_ID_SELECT, MUSE_HOLD_MS, prv_select_hold, NULL);
}

// ---- app ----

static TextLayer *prv_text(ScrollLayer *parent, const char *font_key) {
  TextLayer *text = text_layer_create(GRectZero);
  text_layer_set_font(text, fonts_get_system_font(font_key));
  text_layer_set_background_color(text, GColorClear);
  text_layer_set_overflow_mode(text, GTextOverflowModeWordWrap);
  scroll_layer_add_child(parent, text_layer_get_layer(text));
  return text;
}

static void prv_init(void) {
  MuseAppData *data = app_zalloc_check(sizeof(*data));
  app_state_set_user_data(data);

  data->window = window_create();
  Layer *root = window_get_root_layer(data->window);
  const GRect bounds = layer_get_bounds_by_value(root);

  data->scroll = scroll_layer_create(bounds);
  scroll_layer_set_shadow_hidden(data->scroll, true);
  scroll_layer_set_callbacks(data->scroll, (ScrollLayerCallbacks){
    .click_config_provider = prv_click_config,
  });
  scroll_layer_set_click_config_onto_window(data->scroll, data->window);
  layer_add_child(root, scroll_layer_get_layer(data->scroll));

  data->icon = kino_layer_create(GRectZero);
  scroll_layer_add_child(data->scroll, kino_layer_get_layer(data->icon));
  data->title = prv_text(data->scroll, FONT_KEY_GOTHIC_24_BOLD);
  data->line = prv_text(data->scroll, FONT_KEY_GOTHIC_18);
  data->heard = prv_text(data->scroll, FONT_KEY_GOTHIC_18);
  data->answer = prv_text(data->scroll, FONT_KEY_GOTHIC_24);
  data->footer = prv_text(data->scroll, FONT_KEY_GOTHIC_14);

  data->bar = layer_create(GRect(MUSE_MARGIN * 2, bounds.size.h - MUSE_MARGIN - MUSE_BAR_HEIGHT,
                                 bounds.size.w - MUSE_MARGIN * 4, MUSE_BAR_HEIGHT));
  layer_set_update_proc(data->bar, prv_bar_update_proc);
  layer_add_child(root, data->bar);

  // Opened by a Quick Launch hold: the question is already being heard.
  data->asked = app_launch_reason() == APP_LAUNCH_QUICK_LAUNCH;
  prv_refresh(data);
  app_window_stack_push(data->window, true);
  prv_schedule(data);
}

static void prv_deinit(void) {
  MuseAppData *data = app_state_get_user_data();
  // Leaving mid-question abandons it; a finished answer goes with the screen.
  audio_companion_ptt_cancel();
  audio_companion_assistant_dismiss();
  app_timer_cancel(data->timer);
  text_layer_destroy(data->footer);
  text_layer_destroy(data->answer);
  text_layer_destroy(data->heard);
  text_layer_destroy(data->line);
  text_layer_destroy(data->title);
  kino_layer_destroy(data->icon);
  layer_destroy(data->bar);
  scroll_layer_destroy(data->scroll);
  window_destroy(data->window);
  app_free(data);
}

static void prv_main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}

const PebbleProcessMd *muse_app_get_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common =
        {
          .main_func = prv_main,
          // UUID: 3b8e6f0a-41c2-4d7e-9a35-c0f1e2d4b6a8
          .uuid = {0x3b, 0x8e, 0x6f, 0x0a, 0x41, 0xc2, 0x4d, 0x7e, 0x9a, 0x35, 0xc0, 0xf1, 0xe2,
                   0xd4, 0xb6, 0xa8},
        },
    .name = "Muse",
    .icon_resource_id = RESOURCE_ID_GENERIC_QUESTION_TINY,
  };
  return (const PebbleProcessMd *)&s_app_info;
}

#endif  // CONFIG_SERVICE_AUDIO_COMPANION_PTT
