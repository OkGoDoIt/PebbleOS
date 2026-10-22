/* SPDX-License-Identifier: Apache-2.0 */

#include "muse_app.h"

#ifdef CONFIG_SERVICE_AUDIO_COMPANION_PTT

#include <applib/app.h>
#include <applib/app_launch_button.h>
#include <applib/app_launch_reason.h>
#include <applib/app_timer.h>
#include <applib/fonts/fonts.h>
#include <applib/graphics/graphics.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/kino/kino_layer.h>
#include <applib/ui/action_menu_hierarchy.h>
#include <applib/ui/action_menu_window.h>
#include <applib/ui/animation.h>
#include <applib/ui/property_animation.h>
#include <applib/ui/scroll_layer.h>
#include <applib/ui/status_bar_layer.h>
#include <applib/ui/text_layer.h>
#include <applib/ui/vibes.h>
#include <applib/ui/window.h>
#include <apps/system_app_ids.h>
#include <kernel/pbl_malloc.h>
#include <pbl/drivers/rtc.h>
#include <pbl/services/audio_companion.h>
#include <pbl/services/clock.h>
#include <pbl/services/light.h>
#include <pbl/services/notifications/notification_storage.h>
#include <pbl/services/timeline/attribute.h>
#include <pbl/services/timeline/item.h>
#include <process_state/app_state/app_state.h>
#include <resource/resource_ids.auto.h>
#include <shell/normal/quick_launch.h>

#include <pbl/util/size.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

//! The progress bar moves while listening; everything else changes at the phone's pace.
#define MUSE_POLL_LISTENING_MS (100)
#define MUSE_POLL_MS (250)
//! Holding Select starts a question after the same time a Quick Launch hold takes.
#define MUSE_HOLD_MS (400)
#define MUSE_MARGIN (10)
#define MUSE_GAP (4)
#define MUSE_BAR_HEIGHT (6)
//! The dictation screen's working dot (voice_window.c), so waiting looks the way it does there.
#define MUSE_DOT_MAX_RADIUS (9)
#define MUSE_DOT_MIN_RADIUS (7)
#define MUSE_DOT_OVERSHOOT (4)
#define MUSE_DOT_PULSE_MS (270)
#define MUSE_DOT_DELAY_MS (1000)
#define MUSE_DOT_SIZE (2 * (MUSE_DOT_MAX_RADIUS + MUSE_DOT_OVERSHOOT + 1))
//! Taller than the longest answer the watch keeps (1536 bytes) can lay out.
#define MUSE_TEXT_MAX_HEIGHT (6000)
//! Earlier exchanges of this visit, kept above the current one.
#define MUSE_HISTORY_MAX (3)
//! An exchange older than this shows when it happened.
#define MUSE_SHOW_TIME_AFTER_S (10 * 60)
//! An exchange older than this also shows its day.
#define MUSE_SHOW_DAY_AFTER_S (12 * 60 * 60)
#define MUSE_OPTION_PAD (6)
#define MUSE_OPTION_RADIUS (6)

#define MUSE_ACCENT PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack)
#define MUSE_SECONDARY PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack)

typedef struct {
  uint32_t icon;
  //! Waiting on Muse: the pulsing dot.
  bool working;
  bool accent;  // the listening screen: the mic is on
  bool centered;
  const char *title;
  const char *line;
  //! Who and when, above an exchange: "From Muse", "2:41 PM".
  const char *meta;
  const char *heard;
  const char *answer;
  //! Muse's options, as buttons under the answer.
  bool options;
  const char *footer;
  //! Part of the conversation: earlier exchanges of this visit show above it.
  bool conversation;
} MuseScreen;

//! An exchange of this visit that a newer one replaced, or one that arrived while Muse was
//! closed.
typedef struct {
  char *heard;
  char *answer;
  uint8_t turn;
} MuseExchange;

typedef struct {
  Window *window;
  StatusBarLayer *status_bar;
  ScrollLayer *scroll;
  KinoLayer *icon;
  Layer *dot;
  Animation *dot_anim;
  int16_t dot_radius;
  TextLayer *title;
  TextLayer *line;
  TextLayer *heard;
  TextLayer *answer;
  TextLayer *footer;
  TextLayer *meta;
  Layer *options;
  Layer *divider;
  TextLayer *history_heard[MUSE_HISTORY_MAX];
  TextLayer *history_answer[MUSE_HISTORY_MAX];
  Layer *bar;
  AppTimer *timer;

  //! Oldest first.
  MuseExchange history[MUSE_HISTORY_MAX];
  uint8_t history_count;
  char meta_text[40];
  char footer_text[96];
  //! The options menu: its own copy, as the view may change while it is open.
  ActionMenuConfig menu;
  uint8_t menu_turn;
  char menu_options[AUDIO_COMPANION_ASSISTANT_MAX_OPTIONS][AUDIO_COMPANION_ASSISTANT_OPTION_BYTES];
  //! Opened from a notification: the exchange to show first.
  uint8_t focus_turn;
  //! A pick that couldn't go out, said on the screen it was made from (its generation).
  bool pick_failed;
  uint32_t pick_failed_generation;

  AudioCompanionAssistantView view;
  uint32_t generation;
  bool have_view;
  bool available;
  //! A question was asked from this screen (or brought it up): the next one is a follow-up.
  bool asked;
  //! The button a hold asks with, besides Select: Muse's Quick Launch button, so a hold asks
  //! the same way from the watchface and here. Select when Muse has none.
  ButtonId ask_button;
  uint8_t answered_turn;
  uint8_t max_turn;
  uint32_t shown_icon;
} MuseAppData;

// ---- what to show ----

//! The hints name the button that asks.
static const char *prv_hint(ButtonId button, const char *up, const char *down, const char *select) {
  switch (button) {
    case BUTTON_ID_UP: return up;
    case BUTTON_ID_DOWN: return down;
    default: return select;
  }
}

static const char *prv_ask_again(ButtonId button) {
  return prv_hint(button, "Hold Up to reply", "Hold Down to reply", "Hold Select to reply");
}

//! Under an answer that ends on Muse's options: how to pick one, and how to say something else.
static const char *prv_options_hint(ButtonId button) {
  return prv_hint(button, "Press Select to pick one\nHold Up to reply",
                  "Press Select to pick one\nHold Down to reply",
                  "Press Select to pick one\nHold Select to reply");
}

static const char *prv_try_again(ButtonId button) {
  return prv_hint(button, "Hold Up and try again.", "Hold Down and try again.",
                  "Hold Select and try again.");
}

//! Under the conversation, when the question asked after it held no words.
static const char *prv_did_not_catch(ButtonId button) {
  return prv_hint(button, "Didn't catch that. Hold Up to try again.",
                  "Didn't catch that. Hold Down to try again.",
                  "Didn't catch that. Hold Select to try again.");
}

static const char *prv_speak(ButtonId button) {
  return prv_hint(button, "Hold Up and speak", "Hold Down and speak", "Hold Select and speak");
}

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

static const char *prv_error_line(uint8_t error, ButtonId button) {
  switch (error) {
    case AudioCompanionAssistantErrorNotSetUp: return "Set it up in Pebble Audio on your phone.";
    case AudioCompanionAssistantErrorUnreachable: return "Check your phone's connection.";
    case AudioCompanionAssistantErrorNoAnswer: return "Muse didn't reply. Try again.";
    case AudioCompanionAssistantErrorDidNotCatch: return prv_try_again(button);
    case AudioCompanionAssistantErrorBusy:
    case AUDIO_COMPANION_ASSISTANT_ERROR_MIC_UNAVAILABLE: return "Try again in a moment.";
    case AUDIO_COMPANION_ASSISTANT_ERROR_PHONE_UNAVAILABLE:
    default: return "Muse works through Pebble Audio on your phone.";
  }
}

//! "From Muse" for a message Muse started; the time for an exchange that isn't recent. NULL for
//! neither.
static const char *prv_meta(MuseAppData *data) {
  const AudioCompanionAssistantView *v = &data->view;
  const bool from_muse = v->from_receiver && !v->heard[0];
  const time_t now = rtc_get_time();
  const bool old = v->began_at && now - v->began_at >= MUSE_SHOW_TIME_AFTER_S;
  if (!from_muse && !old) {
    return NULL;
  }
  char when[24] = "";
  if (old) {
    size_t used = 0;
    if (now - v->began_at >= MUSE_SHOW_DAY_AFTER_S) {
      struct tm local;
      localtime_r(&v->began_at, &local);
      used = strftime(when, sizeof(when), "%a ", &local);
    }
    clock_copy_time_string_timestamp(when + used, (uint8_t)(sizeof(when) - used), v->began_at);
  }
  if (from_muse && old) {
    snprintf(data->meta_text, sizeof(data->meta_text), "From Muse \xC2\xB7 %s", when);
  } else if (from_muse) {
    snprintf(data->meta_text, sizeof(data->meta_text), "From Muse");
  } else {
    snprintf(data->meta_text, sizeof(data->meta_text), "%s", when);
  }
  return data->meta_text;
}

static MuseScreen prv_screen_for_phase(MuseAppData *data) {
  const AudioCompanionAssistantView *v = &data->view;
  const char *heard = v->heard[0] ? v->heard : NULL;
  const bool picked_but_failed = data->pick_failed && data->pick_failed_generation == v->generation;
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
        .working = true,
        .centered = true,
        .title = "Sending",
        .line = v->reached_max ? "Questions can run 15 seconds." : NULL,
      };
    case AudioCompanionAssistantPhaseWaiting:
      return (MuseScreen){
        .working = true,
        .centered = data->history_count == 0,
        .title = v->status[0] ? v->status : "Thinking\xE2\x80\xA6",
        .heard = heard,
        // Muse can take its time: nobody has to watch it work.
        .footer = "You can leave. The answer will come as a notification.",
        .conversation = true,
      };
    case AudioCompanionAssistantPhaseAnswering:
    case AudioCompanionAssistantPhaseDone: {
      const bool done = v->phase == AudioCompanionAssistantPhaseDone;
      const bool options = done && v->option_count > 0;
      const char *footer = NULL;
      if (picked_but_failed) {
        footer = "Can't reach your phone. Try again.";
      } else if (done) {
        footer = options ? prv_options_hint(data->ask_button) : prv_ask_again(data->ask_button);
      }
      return (MuseScreen){
        .meta = prv_meta(data),
        .heard = heard,
        .answer = v->answer[0] ? v->answer : "Done.",
        .options = options,
        .footer = footer,
        .conversation = true,
      };
    }
    case AudioCompanionAssistantPhaseError:
      if (v->error == AudioCompanionAssistantErrorStillWorking) {
        // The phone stopped waiting; Muse didn't. Its answer comes as a message later.
        return (MuseScreen){
          .centered = data->history_count == 0,
          .title = "Still working",
          .line = "Muse will send the answer when it's ready.",
          .heard = heard,
          .footer = prv_ask_again(data->ask_button),
          .conversation = true,
        };
      }
      return (MuseScreen){
        .icon = RESOURCE_ID_GENERIC_WARNING_LARGE,
        .centered = true,
        .title = prv_error_title(v->error),
        .line = prv_error_line(v->error, data->ask_button),
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
      .line = prv_error_line(AUDIO_COMPANION_ASSISTANT_ERROR_PHONE_UNAVAILABLE, data->ask_button),
    };
  }
  return (MuseScreen){
    .icon = RESOURCE_ID_VOICE_MICROPHONE_LARGE,
    .centered = true,
    .title = "Ask Muse",
    .line = prv_speak(data->ask_button),
  };
}

static MuseScreen prv_screen(MuseAppData *data) {
  MuseScreen screen = prv_screen_for_phase(data);
  if (data->view.notice == AudioCompanionAssistantErrorDidNotCatch && screen.conversation) {
    screen.footer = prv_did_not_catch(data->ask_button);
  }
  return screen;
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
  // Measured afresh, not through the layer's layout cache: drawing fills that cache with only the
  // lines that fit on screen, and an unchanged answer measured through it came back cut to them.
  const GSize size = graphics_text_layout_get_max_used_size(
      app_state_get_graphics_context(), text, layer->font, GRect(0, 0, width, MUSE_TEXT_MAX_HEIGHT),
      layer->overflow_mode, layer->text_alignment, NULL);
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
  text_layer_set_text_color(data->meta, secondary);
  for (int i = 0; i < MUSE_HISTORY_MAX; i++) {
    text_layer_set_text_color(data->history_heard[i], secondary);
    text_layer_set_text_color(data->history_answer[i], text);
  }
  // The time stays at the top, in the screen's own colors.
  status_bar_layer_set_colors(data->status_bar, accent ? MUSE_ACCENT : GColorWhite, text);
}

// ---- the working dot (as the dictation screen shows while it transcribes) ----

static void prv_dot_update_proc(Layer *layer, GContext *ctx) {
  MuseAppData *data = app_state_get_user_data();
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(MUSE_ACCENT, GColorBlack));
  const GRect bounds = layer_get_bounds_by_value(layer);
  graphics_fill_circle(ctx, grect_center_point(&bounds), data->dot_radius);
}

static void prv_set_dot_radius(void *subject, int16_t radius) {
  MuseAppData *data = subject;
  data->dot_radius = radius;
  layer_mark_dirty(data->dot);
}

static const PropertyAnimationImplementation s_dot_impl = {
  .base = {.update = (AnimationUpdateImplementation)property_animation_update_int16},
  .accessors = {.setter = {.int16 = prv_set_dot_radius}},
};

static Animation *prv_dot_stage(MuseAppData *data, int16_t from, int16_t to) {
  PropertyAnimation *anim = property_animation_create(&s_dot_impl, data, NULL, NULL);
  if (!anim) {
    return NULL;
  }
  property_animation_set_from_int16(anim, &from);
  property_animation_set_to_int16(anim, &to);
  animation_set_duration((Animation *)anim, MUSE_DOT_PULSE_MS / 3);
  animation_set_curve((Animation *)anim, AnimationCurveEaseInOut);
  return (Animation *)anim;
}

static void prv_start_dot(MuseAppData *data) {
  if (data->dot_anim && animation_is_scheduled(data->dot_anim)) {
    return;
  }
  data->dot_radius = MUSE_DOT_MAX_RADIUS;
  // Grow past full size, shrink, return; then rest, and again.
  Animation *expand =
      prv_dot_stage(data, MUSE_DOT_MAX_RADIUS, MUSE_DOT_MAX_RADIUS + MUSE_DOT_OVERSHOOT);
  Animation *shrink =
      prv_dot_stage(data, MUSE_DOT_MAX_RADIUS + MUSE_DOT_OVERSHOOT, MUSE_DOT_MIN_RADIUS);
  Animation *revert = prv_dot_stage(data, MUSE_DOT_MIN_RADIUS, MUSE_DOT_MAX_RADIUS);
  if (!expand || !shrink || !revert) {
    if (expand) {
      animation_destroy(expand);
    }
    if (shrink) {
      animation_destroy(shrink);
    }
    if (revert) {
      animation_destroy(revert);
    }
    return;
  }
  animation_set_delay(expand, MUSE_DOT_DELAY_MS);
  data->dot_anim = animation_sequence_create(expand, shrink, revert, NULL);
  if (!data->dot_anim) {
    return;
  }
  animation_set_play_count(data->dot_anim, ANIMATION_PLAY_COUNT_INFINITE);
  animation_schedule(data->dot_anim);
}

static void prv_stop_dot(MuseAppData *data) {
  if (data->dot_anim) {
    animation_unschedule(data->dot_anim);
    data->dot_anim = NULL;
  }
}

// ---- Muse's options, as the buttons it draws in its own app ----

static GFont prv_option_font(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
}

static int16_t prv_option_text_height(const char *text, int16_t width) {
  return graphics_text_layout_get_max_used_size(app_state_get_graphics_context(), text,
                                                prv_option_font(), GRect(0, 0, width, 1000),
                                                GTextOverflowModeWordWrap, GTextAlignmentLeft,
                                                NULL).h;
}

//! The buttons' total height at `width`, from the top of the first to the bottom of the last.
static int16_t prv_options_height(const MuseAppData *data, int16_t width) {
  int16_t height = 0;
  for (uint8_t i = 0; i < data->view.option_count; i++) {
    if (i) {
      height += MUSE_GAP;
    }
    height += prv_option_text_height(data->view.options[i], width - 2 * MUSE_OPTION_PAD) +
              2 * MUSE_OPTION_PAD;
  }
  return height;
}

static void prv_options_update_proc(Layer *layer, GContext *ctx) {
  MuseAppData *data = app_state_get_user_data();
  const GRect bounds = layer_get_bounds_by_value(layer);
  const int16_t text_width = bounds.size.w - 2 * MUSE_OPTION_PAD;
  int16_t y = 0;
  graphics_context_set_stroke_color(ctx, MUSE_ACCENT);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(MUSE_ACCENT, GColorBlack));
  for (uint8_t i = 0; i < data->view.option_count; i++) {
    const char *option = data->view.options[i];
    const int16_t text = prv_option_text_height(option, text_width);
    const GRect box = GRect(1, y + 1, bounds.size.w - 2, text + 2 * MUSE_OPTION_PAD - 2);
    graphics_draw_round_rect(ctx, &box, MUSE_OPTION_RADIUS);
    // Gothic's ascender leaves room above the letters: nudge them up to sit centred.
    graphics_draw_text(ctx, option, prv_option_font(),
                       GRect(MUSE_OPTION_PAD, y + MUSE_OPTION_PAD - 3, text_width, text + 4),
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    y += text + 2 * MUSE_OPTION_PAD + MUSE_GAP;
  }
}

static void prv_divider_update_proc(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds_by_value(layer);
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack));
  graphics_fill_rect(ctx, &GRect(0, bounds.size.h / 2, bounds.size.w, 1));
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
  text_layer_set_text_alignment(data->footer, align);

  int16_t y = MUSE_MARGIN;
  // This visit's earlier exchanges, oldest first, above the one going on now.
  const bool history = screen.conversation && data->history_count > 0;
  for (int i = 0; i < MUSE_HISTORY_MAX; i++) {
    const bool shown = history && i < data->history_count;
    y = prv_place_text(data->history_heard[i], shown ? data->history[i].heard : NULL, y, width);
    y = prv_place_text(data->history_answer[i], shown ? data->history[i].answer : NULL, y, width);
  }
  if (history) {
    layer_set_frame(data->divider, &GRect(MUSE_MARGIN, y, width, MUSE_MARGIN));
    layer_set_hidden(data->divider, false);
    y += MUSE_MARGIN + MUSE_GAP;
  } else {
    layer_set_hidden(data->divider, true);
  }
  const int16_t current_top = y;
  if (screen.working) {
    layer_set_frame(data->dot, &GRect((bounds.size.w - MUSE_DOT_SIZE) / 2, y, MUSE_DOT_SIZE,
                                      MUSE_DOT_SIZE));
    layer_set_hidden(data->dot, false);
    prv_start_dot(data);
    y += MUSE_DOT_SIZE + MUSE_GAP * 2;
  } else {
    layer_set_hidden(data->dot, true);
    prv_stop_dot(data);
  }
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
  y = prv_place_text(data->meta, screen.meta, y, width);
  y = prv_place_text(data->heard, screen.heard, y, width);
  y = prv_place_text(data->answer, screen.answer, y, width);
  if (screen.options) {
    const int16_t height = prv_options_height(data, width);
    layer_set_frame(data->options, &GRect(MUSE_MARGIN, y, width, height));
    layer_set_hidden(data->options, false);
    layer_mark_dirty(data->options);
    y += height + MUSE_GAP * 2;
  } else {
    layer_set_hidden(data->options, true);
  }
  y = prv_place_text(data->footer, screen.footer, y, width);
  y += MUSE_MARGIN;

  // Below the status bar: short screens sit in the middle; long ones scroll.
  const int16_t top = STATUS_BAR_LAYER_HEIGHT;
  const int16_t visible =
      bounds.size.h - top - (screen.accent ? MUSE_BAR_HEIGHT + MUSE_MARGIN : 0);
  Layer *scroll = scroll_layer_get_layer(data->scroll);
  if (screen.centered && !history && y < visible) {
    layer_set_frame(scroll, &GRect(0, top + (visible - y) / 2, bounds.size.w, y));
  } else {
    layer_set_frame(scroll, &GRect(0, top, bounds.size.w, bounds.size.h - top));
  }
  scroll_layer_set_content_size(data->scroll, GSize(bounds.size.w, y));
  if (scroll_to_top) {
    // A new exchange starts at its own top; Up scrolls back to the earlier ones. Opened from the
    // notification of an earlier one, that one is shown first.
    int16_t offset = history ? current_top - MUSE_GAP : 0;
    for (int i = 0; history && data->focus_turn && i < data->history_count; i++) {
      if (data->history[i].turn == data->focus_turn) {
        offset = layer_get_frame_by_value(text_layer_get_layer(data->history_heard[i])).origin.y;
        if (!data->history[i].heard) {
          offset = layer_get_frame_by_value(text_layer_get_layer(data->history_answer[i])).origin.y;
        }
        offset = MAX(0, offset - MUSE_GAP);
      }
    }
    data->focus_turn = 0;
    scroll_layer_set_content_offset(data->scroll, GPoint(0, -offset), false);
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

static char *prv_copy(const char *text) {
  if (!text || !text[0]) {
    return NULL;
  }
  const size_t size = strlen(text) + 1;
  char *copy = app_malloc(size);
  if (copy) {
    memcpy(copy, text, size);
  }
  return copy;
}

static void prv_free_exchange(MuseExchange *exchange) {
  app_free(exchange->heard);
  app_free(exchange->answer);
  *exchange = (MuseExchange){0};
}

static void prv_add_history(MuseAppData *data, const char *heard, const char *answer,
                            uint8_t turn) {
  if (!answer || !answer[0]) {
    return;
  }
  if (data->history_count == MUSE_HISTORY_MAX) {
    prv_free_exchange(&data->history[0]);
    memmove(&data->history[0], &data->history[1], sizeof(MuseExchange) * (MUSE_HISTORY_MAX - 1));
    data->history[MUSE_HISTORY_MAX - 1] = (MuseExchange){0};
    data->history_count--;
  }
  data->history[data->history_count++] = (MuseExchange){
    .heard = prv_copy(heard),
    .answer = prv_copy(answer),
    .turn = turn,
  };
}

//! The exchange on screen gives way to a newer one: it stays above it for the rest of the visit.
static void prv_keep_in_history(MuseAppData *data, const AudioCompanionAssistantView *old) {
  prv_add_history(data, old->heard, old->answer, old->turn);
}

//! What arrived while Muse was closed, read back from its notifications (oldest first), so the
//! conversation reads as if Muse had never been left. The exchange on screen is shown live.
static void prv_show_missed(MuseAppData *data, const AudioCompanionAssistantMissed *missed,
                            uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    if (data->have_view && missed[i].turn == data->view.turn &&
        data->view.phase != AudioCompanionAssistantPhaseIdle) {
      continue;
    }
    TimelineItem item;
    if (!notification_storage_get(&missed[i].id, &item)) {
      continue;
    }
    prv_add_history(data, attribute_get_string(&item.attr_list, AttributeIdSubtitle, ""),
                    attribute_get_string(&item.attr_list, AttributeIdBody, ""), missed[i].turn);
    timeline_item_free_allocated_buffer(&item);
  }
}

//! The exchange now on screen is the newest one kept above it, given back by a question that came
//! to nothing: it moves back down, and its answer, already read, doesn't tap the wrist again.
static void prv_take_back_from_history(MuseAppData *data) {
  const AudioCompanionAssistantView *v = &data->view;
  if (data->history_count == 0 || v->phase == AudioCompanionAssistantPhaseIdle ||
      data->history[data->history_count - 1].turn != v->turn) {
    return;
  }
  data->history_count--;
  prv_free_exchange(&data->history[data->history_count]);
  data->answered_turn = v->turn;
}

//! Opened on a question just begun (a Quick Launch hold): the exchange it follows shows above it,
//! as if Muse had been open all along.
static void prv_show_before(MuseAppData *data) {
  const AudioCompanionAssistantPhase phase = data->view.phase;
  if (phase != AudioCompanionAssistantPhaseListening &&
      phase != AudioCompanionAssistantPhaseSending &&
      phase != AudioCompanionAssistantPhaseWaiting) {
    return;
  }
  AudioCompanionAssistantView *before = app_malloc(sizeof(*before));
  if (!before) {
    return;
  }
  if (audio_companion_assistant_get_before(before) && before->turn != data->view.turn) {
    prv_keep_in_history(data, before);
  }
  app_free(before);
}

//! Whether `next` is a different exchange from `old` rather than more of it: another turn, or
//! a reply on the same turn (a picked option, a follow-up) that cleared its answer.
static bool prv_new_exchange(const AudioCompanionAssistantView *old,
                             const AudioCompanionAssistantView *next) {
  if (next->turn != old->turn) {
    return next->phase != AudioCompanionAssistantPhaseIdle;
  }
  return old->phase == AudioCompanionAssistantPhaseDone &&
         next->phase == AudioCompanionAssistantPhaseWaiting;
}

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
  if (data->have_view) {
    AudioCompanionAssistantView *next = app_malloc(sizeof(*next));
    if (next) {
      audio_companion_assistant_get_view(next);
      if (prv_new_exchange(&data->view, next)) {
        prv_keep_in_history(data, &data->view);
      }
      data->view = *next;
      app_free(next);
    } else {
      audio_companion_assistant_get_view(&data->view);
    }
  } else {
    audio_companion_assistant_get_view(&data->view);
  }
  data->generation = generation;
  const bool first = !data->have_view;
  data->have_view = true;
  if (first) {
    prv_show_before(data);
  } else if (data->view.turn != was_turn) {
    prv_take_back_from_history(data);
  }
  const AudioCompanionAssistantView *v = &data->view;
  if (v->phase != AudioCompanionAssistantPhaseIdle && v->turn > data->max_turn) {
    data->max_turn = v->turn;
    data->asked = true;
  }

  if (v->phase == AudioCompanionAssistantPhaseWaiting && data->answered_turn == v->turn) {
    // A follow-up on this screen: its answer taps the wrist too.
    data->answered_turn = 0;
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

static void prv_ask_down(ClickRecognizerRef recognizer, void *context) {
  MuseAppData *data = app_state_get_user_data();
  // Listening starts at the press so the first words are kept; it only becomes a question once
  // the hold lasts (prv_ask_hold). The release is handled by the service, wherever it lands.
  audio_companion_ptt_arm(click_recognizer_get_button_id(recognizer),
                          data->asked ? AudioCompanionPttTriggerFollowUp
                                      : AudioCompanionPttTriggerInApp);
}

static void prv_ask_hold(ClickRecognizerRef recognizer, void *context) {
  MuseAppData *data = app_state_get_user_data();
  audio_companion_ptt_commit();
  prv_refresh(data);
}

static void prv_option_picked(ActionMenu *menu, const ActionMenuItem *item, void *context) {
  MuseAppData *data = context;
  const uint8_t index = (uint8_t)(uintptr_t)action_menu_item_get_action_data(item);
  if (!audio_companion_assistant_pick(data->menu_turn, index)) {
    data->pick_failed = true;
    data->pick_failed_generation = data->view.generation;
    vibes_double_pulse();
    prv_layout(data, false);
    return;
  }
  prv_refresh(data);
}

static void prv_options_menu_closed(ActionMenu *menu, const ActionMenuItem *item, void *context) {
  action_menu_hierarchy_destroy(action_menu_get_root_level(menu), NULL, NULL);
}

//! Select, on an answer that ends on Muse's options: the same menu its notification shows.
static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  MuseAppData *data = app_state_get_user_data();
  const AudioCompanionAssistantView *v = &data->view;
  if (v->phase != AudioCompanionAssistantPhaseDone || v->option_count == 0) {
    return;
  }
  ActionMenuLevel *level = action_menu_level_create(v->option_count);
  if (!level) {
    return;
  }
  data->menu_turn = v->turn;
  memcpy(data->menu_options, v->options, sizeof(data->menu_options));
  for (uint8_t i = 0; i < v->option_count; i++) {
    if (data->menu_options[i][0]) {
      action_menu_level_add_action(level, data->menu_options[i], prv_option_picked,
                                   (void *)(uintptr_t)i);
    }
  }
  data->menu = (ActionMenuConfig){
    .root_level = level,
    .context = data,
    .colors = {.background = MUSE_ACCENT, .foreground = GColorWhite},
    .did_close = prv_options_menu_closed,
  };
  app_action_menu_open(&data->menu);
}

//! The button Muse is on in Quick Launch: the one that opened it, or else the first that has it.
static ButtonId prv_ask_button(void) {
  if (app_launch_reason() == APP_LAUNCH_QUICK_LAUNCH) {
    const ButtonId launched = app_launch_button();
    if (launched == BUTTON_ID_UP || launched == BUTTON_ID_DOWN) {
      return launched;
    }
  }
  const ButtonId buttons[] = { BUTTON_ID_UP, BUTTON_ID_DOWN };
  for (size_t i = 0; i < ARRAY_LENGTH(buttons); i++) {
    if (quick_launch_is_enabled(buttons[i]) && quick_launch_get_app(buttons[i]) == APP_ID_MUSE) {
      return buttons[i];
    }
  }
  return BUTTON_ID_SELECT;
}

static void prv_click_config(void *context) {
  MuseAppData *data = context;
  window_raw_click_subscribe(BUTTON_ID_SELECT, prv_ask_down, NULL, NULL);
  window_long_click_subscribe(BUTTON_ID_SELECT, MUSE_HOLD_MS, prv_ask_hold, NULL);
  // A press shorter than the hold: Muse's options, when the answer has them.
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
  if (data->ask_button != BUTTON_ID_SELECT) {
    // A press still scrolls; a hold asks (it no longer scrolls fast).
    window_raw_click_subscribe(data->ask_button, prv_ask_down, NULL, NULL);
    window_long_click_subscribe(data->ask_button, MUSE_HOLD_MS, prv_ask_hold, NULL);
  }
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
  data->ask_button = prv_ask_button();
  scroll_layer_set_context(data->scroll, data);
  scroll_layer_set_callbacks(data->scroll, (ScrollLayerCallbacks){
    .click_config_provider = prv_click_config,
  });
  scroll_layer_set_click_config_onto_window(data->scroll, data->window);
  layer_add_child(root, scroll_layer_get_layer(data->scroll));

  data->icon = kino_layer_create(GRectZero);
  scroll_layer_add_child(data->scroll, kino_layer_get_layer(data->icon));
  data->dot = layer_create(GRectZero);
  layer_set_update_proc(data->dot, prv_dot_update_proc);
  layer_set_hidden(data->dot, true);
  scroll_layer_add_child(data->scroll, data->dot);
  data->title = prv_text(data->scroll, FONT_KEY_GOTHIC_24_BOLD);
  data->line = prv_text(data->scroll, FONT_KEY_GOTHIC_18);
  data->heard = prv_text(data->scroll, FONT_KEY_GOTHIC_18);
  data->answer = prv_text(data->scroll, FONT_KEY_GOTHIC_24);
  data->footer = prv_text(data->scroll, FONT_KEY_GOTHIC_14);
  data->meta = prv_text(data->scroll, FONT_KEY_GOTHIC_14);
  for (int i = 0; i < MUSE_HISTORY_MAX; i++) {
    data->history_heard[i] = prv_text(data->scroll, FONT_KEY_GOTHIC_14);
    data->history_answer[i] = prv_text(data->scroll, FONT_KEY_GOTHIC_18);
  }
  data->divider = layer_create(GRectZero);
  layer_set_update_proc(data->divider, prv_divider_update_proc);
  scroll_layer_add_child(data->scroll, data->divider);
  data->options = layer_create(GRectZero);
  layer_set_update_proc(data->options, prv_options_update_proc);
  scroll_layer_add_child(data->scroll, data->options);

  data->bar = layer_create(GRect(MUSE_MARGIN * 2, bounds.size.h - MUSE_MARGIN - MUSE_BAR_HEIGHT,
                                 bounds.size.w - MUSE_MARGIN * 4, MUSE_BAR_HEIGHT));
  layer_set_update_proc(data->bar, prv_bar_update_proc);
  layer_add_child(root, data->bar);

  // The standard status bar, so the time is always there.
  data->status_bar = status_bar_layer_create();
  layer_add_child(root, status_bar_layer_get_layer(data->status_bar));

  // Opened by a Quick Launch hold: the question is already being heard. Opened from a
  // notification: it continues the conversation it came from.
  const AppLaunchReason reason = app_launch_reason();
  data->asked = reason == APP_LAUNCH_QUICK_LAUNCH || reason == APP_LAUNCH_TIMELINE_ACTION;
  AudioCompanionAssistantMissed missed[MUSE_HISTORY_MAX + 1];
  const uint8_t missed_count = audio_companion_assistant_screen_opened(missed, ARRAY_LENGTH(missed));
  if (reason == APP_LAUNCH_TIMELINE_ACTION) {
    const uint32_t code = app_launch_get_args();
    if (code & AUDIO_COMPANION_ASSISTANT_LAUNCH_PICK) {
      // An option picked on the notification: it goes to Muse, and its answer comes here. One
      // whose answer has moved on opens the conversation as it is.
      audio_companion_assistant_pick((uint8_t)(code >> 8), (uint8_t)code);
    } else if (code & AUDIO_COMPANION_ASSISTANT_LAUNCH_OPEN_AT) {
      data->focus_turn = (uint8_t)(code >> 8);
    }
  }
  prv_refresh(data);
  if (missed_count) {
    prv_show_missed(data, missed, missed_count);
    prv_layout(data, true);
    // Shown here now: like a message read on the phone, their notifications go.
    audio_companion_assistant_take_back(missed, missed_count);
  }
  app_window_stack_push(data->window, true);
  prv_schedule(data);
}

static void prv_deinit(void) {
  MuseAppData *data = app_state_get_user_data();
  // A question still held is abandoned; one already sent keeps going, and its answer comes as a
  // notification.
  audio_companion_assistant_screen_closed();
  app_timer_cancel(data->timer);
  prv_stop_dot(data);
  layer_destroy(data->dot);
  for (int i = 0; i < MUSE_HISTORY_MAX; i++) {
    prv_free_exchange(&data->history[i]);
    text_layer_destroy(data->history_answer[i]);
    text_layer_destroy(data->history_heard[i]);
  }
  layer_destroy(data->options);
  layer_destroy(data->divider);
  text_layer_destroy(data->meta);
  status_bar_layer_destroy(data->status_bar);
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
          .uuid = MUSE_APP_UUID,
        },
    .name = "Muse",
    .icon_resource_id = RESOURCE_ID_GENERIC_QUESTION_TINY,
  };
  return (const PebbleProcessMd *)&s_app_info;
}

#endif  // CONFIG_SERVICE_AUDIO_COMPANION_PTT
