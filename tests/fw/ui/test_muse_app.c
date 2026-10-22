/* SPDX-License-Identifier: Apache-2.0 */

// The Muse app's screens, rendered from views the audio companion service would hand it. Each
// test writes its screen to the test output directory, for people to look at; the checks are on
// what each screen says.

#include <stdio.h>
#include <string.h>

#include <applib/graphics/framebuffer.h>
#include <applib/graphics/graphics.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/kino/kino_reel.h>
#include <applib/ui/text_layer.h>
#include <applib/ui/window_private.h>
#include <clar.h>
#include <resource/resource.h>
#include <resource/resource_ids.auto.h>
#include <shell/system_theme.h>

static GContext s_ctx;

// Fakes
/////////////////////

#include <fake_content_indicator.h>
#include <fake_spi_flash.h>
#include <fixtures/load_test_resources.h>

GContext *graphics_context_get_current_context(void) {
  return &s_ctx;
}

// Stubs
/////////////////////

#include <stubs_analytics.h>
#include <stubs_app_install_manager.h>
#include <stubs_app_state.h>
#include <stubs_app_timer.h>
#include <stubs_bootbits.h>
#include <stubs_buffer.h>
#include <stubs_click.h>
#include <stubs_heap.h>
#include <stubs_logging.h>
#include <stubs_memory_layout.h>
#include <stubs_mutex.h>
#include <stubs_passert.h>
#include <stubs_pbl_malloc.h>
#include <stubs_pebble_tasks.h>
#include <stubs_print.h>
#include <stubs_process_manager.h>
#include <stubs_serial.h>
#include <stubs_shell_prefs.h>
#include <stubs_sleep.h>
#include <stubs_syscall_internal.h>
#include <stubs_syscalls.h>
#include <stubs_task_wdt.h>
#include <stubs_unobstructed_area.h>
#include <stubs_vibes.h>
#include <stubs_window_manager.h>
#include <stubs_window_stack.h>

AnimationProgress animation_timing_scaled(AnimationProgress time_normalized,
                                          AnimationProgress interval_start,
                                          AnimationProgress interval_end) {
  return interval_end;
}

AnimationProgress animation_timing_curve(AnimationProgress time_normalized, AnimationCurve curve) {
  return time_normalized;
}

KinoReel *kino_reel_scale_segmented_create(KinoReel *from_reel, bool take_ownership,
                                           GRect screen_frame) {
  return NULL;
}

void kino_reel_scale_segmented_set_deflate_effect(KinoReel *reel, int16_t expand) {
}

bool kino_reel_scale_segmented_set_delay_by_distance(KinoReel *reel, GPoint target) {
  return false;
}

uint16_t time_ms(time_t *tloc, uint16_t *out_ms) {
  return 0;
}

// The audio companion service, as the app sees it
/////////////////////

#include <pbl/services/audio_companion.h>

static AudioCompanionAssistantView s_view;
static int s_picks;

bool audio_companion_ptt_available(void) {
  return true;
}

uint32_t audio_companion_assistant_get_view(AudioCompanionAssistantView *out) {
  if (out) {
    *out = s_view;
  }
  return s_view.generation;
}

bool audio_companion_ptt_arm(ButtonId button, AudioCompanionPttTrigger trigger) {
  return true;
}

void audio_companion_ptt_commit(void) {}

static bool s_have_before;
static AudioCompanionAssistantView s_before;

bool audio_companion_assistant_get_before(AudioCompanionAssistantView *out) {
  if (s_have_before) {
    *out = s_before;
  }
  return s_have_before;
}
static AudioCompanionAssistantMissed s_missed[2];
static uint8_t s_missed_count;
static int s_taken_back;

uint8_t audio_companion_assistant_screen_opened(AudioCompanionAssistantMissed *missed,
                                                uint8_t max) {
  const uint8_t count = MIN(max, s_missed_count);
  memcpy(missed, s_missed, sizeof(s_missed[0]) * count);
  return count;
}

void audio_companion_assistant_take_back(const AudioCompanionAssistantMissed *missed,
                                         uint8_t count) {
  s_taken_back += count;
}

void audio_companion_assistant_screen_closed(void) {}

// The notifications Muse posted while closed, as storage holds them.
#include <pbl/services/timeline/item.h>
static const char *s_missed_heard = "remind me to get coffee";
static const char *s_missed_body = "Done \xE2\x80\x94 set for 5pm today (Oct 21, 17:00).";

bool notification_storage_get(const Uuid *id, TimelineItem *item_out) {
  memset(item_out, 0, sizeof(*item_out));
  return true;
}

void timeline_item_free_allocated_buffer(TimelineItem *item) {}

const char *attribute_get_string(const AttributeList *attr_list, AttributeId id,
                                 char *default_value) {
  return id == AttributeIdSubtitle ? s_missed_heard : s_missed_body;
}

bool audio_companion_assistant_pick(uint8_t turn, uint8_t index) {
  s_picks++;
  return true;
}

// The rest of the system around it
/////////////////////

#include <applib/app_launch_reason.h>
#include <applib/ui/action_menu_hierarchy.h>
#include <applib/ui/action_menu_window.h>

AppLaunchReason app_launch_reason(void) {
  return APP_LAUNCH_USER;
}

ButtonId app_launch_button(void) {
  return BUTTON_ID_SELECT;
}

uint32_t app_launch_get_args(void) {
  return 0;
}

bool quick_launch_is_enabled(ButtonId button) {
  return button == BUTTON_ID_DOWN;
}

#define APP_ID_MUSE ((AppInstallId)-140)

AppInstallId quick_launch_get_app(ButtonId button) {
  return APP_ID_MUSE;
}

void light_enable_interaction(void) {}

void app_window_stack_push(Window *window, bool animated) {}

ActionMenu *app_action_menu_open(ActionMenuConfig *config) {
  return NULL;
}

ActionMenuLevel *action_menu_level_create(uint16_t max_items) {
  return NULL;
}

ActionMenuItem *action_menu_level_add_action(ActionMenuLevel *level, const char *label,
                                             ActionMenuPerformActionCb cb, void *action_data) {
  return NULL;
}

void action_menu_hierarchy_destroy(const ActionMenuLevel *root, ActionMenuEachItemCb each_cb,
                                   void *context) {}

void *action_menu_item_get_action_data(const ActionMenuItem *item) {
  return NULL;
}

ActionMenuLevel *action_menu_get_root_level(ActionMenu *action_menu) {
  return NULL;
}

size_t clock_copy_time_string_timestamp(char *buffer, uint8_t size, time_t timestamp) {
  return snprintf(buffer, size, "2:41 PM");
}

void app_event_loop(void) {}

void clock_copy_time_string(char *buffer, uint8_t size) {
  snprintf(buffer, size, "2:41 PM");
}

#include <applib/event_service_client.h>
void event_service_client_subscribe(EventServiceInfo *service_info) {}
void event_service_client_unsubscribe(EventServiceInfo *service_info) {}

#include <applib/ui/property_animation.h>
bool property_animation_from(PropertyAnimation *property_animation, void *from, size_t size,
                             bool set) {
  return false;
}

#define CONFIG_SERVICE_AUDIO_COMPANION_PTT 1
#include "apps/system/muse/muse_app.c"

// Helper Functions
/////////////////////

#include "../graphics/test_graphics.h"
#include "../graphics/util.h"

static FrameBuffer *s_fb;
static MuseAppData *s_data;

void test_muse_app__initialize(void) {
  s_fb = malloc(sizeof(FrameBuffer));
  framebuffer_init(s_fb, &(GSize){DISP_COLS, DISP_ROWS});
  graphics_context_init(&s_ctx, s_fb, GContextInitializationMode_System);
  framebuffer_clear(s_fb);
  s_app_state_get_graphics_context = &s_ctx;

  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);
  pfs_format(true /* write erase headers */);
  load_resource_fixture_in_flash(RESOURCES_FIXTURE_PATH, SYSTEM_RESOURCES_FIXTURE_NAME,
                                 false /* is_next */);
  resource_init();

  memset(&s_view, 0, sizeof(s_view));
  s_picks = 0;
  s_missed_count = 0;
  s_taken_back = 0;
  s_have_before = false;
  rtc_set_time(1760000000);
}

//! Opens the app, as the wearer would, after the test has set what it finds.
static void prv_open(void) {
  prv_init();
  s_data = app_state_get_user_data();
}

void test_muse_app__cleanup(void) {
  free(s_fb);
}

//! Shows `view` (a new generation of it) and renders the screen.
static void prv_show(const AudioCompanionAssistantView *view) {
  const uint32_t generation = s_view.generation + 1;
  s_view = *view;
  s_view.generation = generation;
  prv_refresh(s_data);
  window_set_on_screen(s_data->window, true, true);
  window_render(s_data->window, &s_ctx);
}

static void prv_save(const char *name) {
  char filename[96];
  snprintf(filename, sizeof(filename), "muse_app_%s.pbi", name);
  cl_check(tests_write_gbitmap_to_pbi(&s_ctx.dest_bitmap, filename));
}

static AudioCompanionAssistantView prv_view(uint8_t turn, AudioCompanionAssistantPhase phase,
                                            const char *heard, const char *answer) {
  AudioCompanionAssistantView view = {.turn = turn, .phase = phase, .max_seconds = 15};
  if (heard) {
    strncpy(view.heard, heard, sizeof(view.heard) - 1);
  }
  if (answer) {
    strncpy(view.answer, answer, sizeof(view.answer) - 1);
  }
  view.began_at = rtc_get_time();
  return view;
}

static const char *prv_shown(TextLayer *layer) {
  return layer_get_hidden(text_layer_get_layer(layer)) ? NULL : text_layer_get_text(layer);
}

// Tests
//////////////////////

void test_muse_app__ask(void) {
  prv_open();
  AudioCompanionAssistantView view = {0};
  prv_show(&view);
  cl_assert_equal_s(prv_shown(s_data->title), "Ask Muse");
  cl_assert_equal_s(prv_shown(s_data->line), "Hold Down and speak");
  prv_save("ask");
}

void test_muse_app__thinking_says_you_can_leave(void) {
  prv_open();
  AudioCompanionAssistantView view =
      prv_view(3, AudioCompanionAssistantPhaseWaiting, "how far did I walk yesterday \xF0\x9F\x91\x8D", NULL);
  prv_show(&view);
  cl_assert_equal_s(prv_shown(s_data->footer),
                    "You can leave. The answer will come as a notification.");
  prv_save("thinking");
}

void test_muse_app__an_answer_with_options_shows_them_as_buttons(void) {
  prv_open();
  AudioCompanionAssistantView view =
      prv_view(3, AudioCompanionAssistantPhaseDone, "quiz me", "What is 2 times 7?");
  view.option_count = 3;
  strcpy(view.options[0], "A: 12");
  strcpy(view.options[1], "B: 14");
  strcpy(view.options[2], "C: 16");
  prv_show(&view);
  cl_assert(!layer_get_hidden(s_data->options));
  cl_assert_equal_s(prv_shown(s_data->footer), "Press Select to pick one\nHold Down to reply");
  prv_save("options");
}

void test_muse_app__a_follow_up_keeps_the_last_exchange_above(void) {
  prv_open();
  AudioCompanionAssistantView first =
      prv_view(3, AudioCompanionAssistantPhaseDone, "how far did I walk yesterday",
               "You walked 6.2 km yesterday, about 8,100 steps.");
  prv_show(&first);
  AudioCompanionAssistantView second =
      prv_view(4, AudioCompanionAssistantPhaseDone, "and the day before",
               "4.8 km the day before, so yesterday was a little more.");
  prv_show(&second);
  cl_assert_equal_i(s_data->history_count, 1);
  cl_assert_equal_s(s_data->history[0].heard, "how far did I walk yesterday");
  cl_assert_equal_s(prv_shown(s_data->footer), "Hold Down to reply");
  prv_save("follow_up");
  // Up shows the earlier exchange.
  scroll_layer_set_content_offset(s_data->scroll, GPointZero, false);
  window_render(s_data->window, &s_ctx);
  prv_save("follow_up_scrolled_up");
}

void test_muse_app__a_message_muse_started_says_who_it_is_from(void) {
  prv_open();
  AudioCompanionAssistantView view =
      prv_view(200, AudioCompanionAssistantPhaseDone, NULL,
               "Your 3 PM with Sam moved to 4. I updated your calendar.");
  view.from_receiver = true;
  view.began_at = rtc_get_time() - 20 * 60;
  prv_show(&view);
  cl_assert_equal_s(prv_shown(s_data->meta), "From Muse \xC2\xB7 2:41 PM");
  prv_save("from_muse");
}

void test_muse_app__an_answer_handed_to_the_phone_says_so(void) {
  prv_open();
  AudioCompanionAssistantView view = prv_view(
      5, AudioCompanionAssistantPhaseDone, "find me the passport renewal form",
      "You can renew by mail with form DS-82 if your last passport is under 15 years old.\n\n"
      "Link sent to your phone");
  prv_show(&view);
  prv_save("handoff");
}

void test_muse_app__still_working(void) {
  prv_open();
  AudioCompanionAssistantView view =
      prv_view(6, AudioCompanionAssistantPhaseError, "book a table for two at 7", NULL);
  view.error = AudioCompanionAssistantErrorStillWorking;
  prv_show(&view);
  cl_assert_equal_s(prv_shown(s_data->title), "Still working");
  cl_assert_equal_s(prv_shown(s_data->line), "Muse will send the answer when it's ready.");
  prv_save("still_working");
}

//! Roger's 12:01 answer, which the watch showed cut off after "for the 6 full".
static const char *const s_long_answer =
    "Done \xE2\x80\x94 clean overview, last 7 days, Pebble data only.\n\n"
    "Summary (Oct 15\xE2\x80\x93" "21)\n"
    "\xE2\x80\xA2 Steps: 83,259 total, 11,894/day average (12,715/day for the 6 full days)\n"
    "\xE2\x80\xA2 Distance: 76.5 km total (47.6 miles), 10.9 km/day average (11.7 km/day for full "
    "days)\n"
    "\xE2\x80\xA2 Daily: Thu 10,133 steps / 9.34 km \xC2\xB7 Fri 19,598 / 18.34 km \xC2\xB7 Sat "
    "12,713 / 11.64 km \xC2\xB7 Sun 12,492 / 11.80 km \xC2\xB7 Mon 12,116 / 10.56 km \xC2\xB7 Tue "
    "9,237 / 8.53 km \xC2\xB7 Wed so far 6,970 / 6.34 km\n"
    "\xE2\x80\xA2 Heart rate: no data \xE2\x80\x94 the watch returned \xE2\x80\x9Cno heart_rate "
    "data\xE2\x80\x9D for both 7-day and 1-day reads, so there are no heart rate values to "
    "report\n\n"
    "The detailed report is attached below.";

void test_muse_app__a_long_answer_is_laid_out_whole(void) {
  prv_open();
  AudioCompanionAssistantView view =
      prv_view(2, AudioCompanionAssistantPhaseDone, "Clean overview only", s_long_answer);
  prv_show(&view);
  const GRect answer = layer_get_frame_by_value(text_layer_get_layer(s_data->answer));
  const GSize needed = text_layer_get_content_size(&s_ctx, s_data->answer);
  printf("answer frame h=%d, content h=%d, footer y=%d, scroll content h=%d\n", answer.size.h,
         needed.h, layer_get_frame_by_value(text_layer_get_layer(s_data->footer)).origin.y,
         scroll_layer_get_content_size(s_data->scroll).h);
  cl_assert(answer.size.h >= needed.h);
  prv_save("long_top");
  // Scrolled to the end: the last line shows above the footer.
  scroll_layer_set_content_offset(
      s_data->scroll, GPoint(0, -(scroll_layer_get_content_size(s_data->scroll).h - 200)), false);
  window_render(s_data->window, &s_ctx);
  prv_save("long_bottom");
}

void test_muse_app__a_streamed_answer_is_laid_out_whole(void) {
  prv_open();
  // As the watch sees it: the answer grows a few words at a time, then is done.
  AudioCompanionAssistantView view =
      prv_view(2, AudioCompanionAssistantPhaseWaiting, "Clean overview only", NULL);
  prv_show(&view);
  const size_t total = strlen(s_long_answer);
  for (size_t used = 0; used < total;) {
    used = MIN(total, used + 23);
    // Never split a character.
    while (used < total && (((uint8_t)s_long_answer[used]) & 0xC0) == 0x80) {
      used++;
    }
    view.phase = AudioCompanionAssistantPhaseAnswering;
    memcpy(view.answer, s_long_answer, used);
    view.answer[used] = '\0';
    prv_show(&view);
  }
  view.phase = AudioCompanionAssistantPhaseDone;
  prv_show(&view);
  const GRect answer = layer_get_frame_by_value(text_layer_get_layer(s_data->answer));
  const GSize needed = text_layer_get_content_size(&s_ctx, s_data->answer);
  printf("streamed: answer frame h=%d, content h=%d\n", answer.size.h, needed.h);
  cl_assert(answer.size.h >= needed.h);
  scroll_layer_set_content_offset(
      s_data->scroll, GPoint(0, -(scroll_layer_get_content_size(s_data->scroll).h - 200)), false);
  window_render(s_data->window, &s_ctx);
  prv_save("streamed_bottom");
}

void test_muse_app__what_came_while_away_reads_as_one_conversation(void) {
  // A reminder was set while Muse was closed; then Muse wrote again. Opening Muse shows both.
  s_missed[0] = (AudioCompanionAssistantMissed){.turn = 4};
  s_missed[1] = (AudioCompanionAssistantMissed){.turn = 5};
  s_missed_count = 2;
  s_view = prv_view(5, AudioCompanionAssistantPhaseDone, NULL,
                    "Also: the specialty coffee place closes at 6 today.");
  s_view.from_receiver = true;
  s_view.generation = 1;
  prv_open();
  cl_assert_equal_i(s_data->history_count, 1);
  cl_assert_equal_s(s_data->history[0].heard, "remind me to get coffee");
  cl_assert_equal_i(s_taken_back, 2);
  window_set_on_screen(s_data->window, true, true);
  window_render(s_data->window, &s_ctx);
  prv_save("while_away");
}

//! Muse opened by a Quick Launch hold, on the question just begun after "how far did I walk".
static void prv_open_on_a_new_question(void) {
  s_before = prv_view(3, AudioCompanionAssistantPhaseDone, "how far did I walk yesterday",
                      "You walked 6.2 km yesterday, about 8,100 steps.");
  s_have_before = true;
  s_view = prv_view(4, AudioCompanionAssistantPhaseListening, NULL, NULL);
  s_view.generation = 1;
  prv_open();
}

void test_muse_app__a_quick_launch_hold_keeps_the_conversation_above_the_question(void) {
  prv_open_on_a_new_question();
  cl_assert_equal_i(s_data->history_count, 1);
  cl_assert_equal_s(s_data->history[0].answer, "You walked 6.2 km yesterday, about 8,100 steps.");
  // Heard and thinking: the earlier exchange sits above it.
  s_have_before = false;
  AudioCompanionAssistantView waiting =
      prv_view(4, AudioCompanionAssistantPhaseWaiting, "and the day before", NULL);
  prv_show(&waiting);
  cl_assert_equal_i(s_data->history_count, 1);
  scroll_layer_set_content_offset(s_data->scroll, GPointZero, false);
  window_render(s_data->window, &s_ctx);
  prv_save("quick_launch_thinking");
}

void test_muse_app__a_hold_released_without_a_question_shows_the_conversation(void) {
  prv_open_on_a_new_question();
  // The service gives the earlier exchange back: it is the one on screen again, not twice.
  s_have_before = false;
  prv_show(&s_before);
  cl_assert_equal_i(s_data->history_count, 0);
  cl_assert_equal_s(prv_shown(s_data->answer), "You walked 6.2 km yesterday, about 8,100 steps.");
  cl_assert_equal_s(prv_shown(s_data->footer), "Hold Down to reply");
  // Already read: no tap on the wrist for it.
  cl_assert_equal_i(s_data->answered_turn, 3);
  prv_save("released_without_a_question");
}

void test_muse_app__a_question_with_no_words_says_so_under_the_conversation(void) {
  prv_open_on_a_new_question();
  s_have_before = false;
  AudioCompanionAssistantView back = s_before;
  back.notice = AudioCompanionAssistantErrorDidNotCatch;
  prv_show(&back);
  cl_assert_equal_i(s_data->history_count, 0);
  cl_assert_equal_s(prv_shown(s_data->answer), "You walked 6.2 km yesterday, about 8,100 steps.");
  cl_assert_equal_s(prv_shown(s_data->footer), "Didn't catch that. Hold Down to try again.");
  prv_save("did_not_catch");
}
