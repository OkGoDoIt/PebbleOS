/* SPDX-License-Identifier: Apache-2.0 */

#include "audio_companion_actions_ui.h"

#ifdef CONFIG_SERVICE_AUDIO_COMPANION_PTT

#include "applib/fonts/fonts.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/text.h"
#include "applib/ui/dialogs/dialog.h"
#include "applib/ui/dialogs/simple_dialog.h"
#include "applib/ui/menu_layer.h"
#include "applib/ui/vibes.h"
#include "applib/ui/window.h"
#include "applib/ui/window_stack.h"
#include "apps/system/muse/muse_app.h"
#include "kernel/pbl_malloc.h"
#include "kernel/pebble_tasks.h"
#include "kernel/ui/kernel_ui.h"
#include "kernel/ui/modals/modal_manager.h"
#include "pbl/services/evented_timer.h"
#include "pbl/services/light.h"
#include "pbl/services/notifications/notification_storage.h"
#include "pbl/services/notifications/notifications.h"
#include "pbl/services/timeline/attribute.h"
#include "pbl/services/timeline/item.h"
#include "pbl/services/timeline/timeline.h"
#include <pbl/drivers/rtc.h>
#include "resource/resource_ids.auto.h"
#include "resource/timeline_resource_ids.auto.h"
#include "services/audio_companion/assistant_view.h"
#include "services/audio_companion/watch_actions.h"
#include "system/passert.h"

#include <stdio.h>
#include <string.h>

#define ACTIONS_MARGIN (8)
//! A finished timer buzzes every 2 s for half a minute, or until it is dismissed.
#define TIMER_BUZZ_INTERVAL_MS (2000)
#define TIMER_BUZZ_COUNT (15)

#define ACTIONS_ACCENT PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack)

// ---- a timer ended ----

typedef struct {
  SimpleDialog *dialog;
  EventedTimerID buzz_timer;
  int buzzes;
  char text[AUDIO_COMPANION_WATCH_ACTION_LABEL_MAX_BYTES + 16];
} TimerCard;

static void prv_timer_buzz_cb(void *data) {
  TimerCard *card = data;
  if (++card->buzzes >= TIMER_BUZZ_COUNT) {
    evented_timer_cancel(card->buzz_timer);
    card->buzz_timer = EVENTED_TIMER_INVALID_ID;
    return;
  }
  vibes_long_pulse();
}

static void prv_timer_card_unload(void *context) {
  TimerCard *card = context;
  if (card->buzz_timer != EVENTED_TIMER_INVALID_ID) {
    evented_timer_cancel(card->buzz_timer);
  }
  vibes_cancel();
  task_free(card);
}

static void prv_timer_done_handler(const char *label) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  TimerCard *card = task_zalloc_check(sizeof(*card));
  if (label && label[0]) {
    snprintf(card->text, sizeof(card->text), "%s\nTimer done", label);
  } else {
    strncpy(card->text, "Timer done", sizeof(card->text) - 1);
  }
  card->dialog = simple_dialog_create("Muse Timer");
  Dialog *dialog = simple_dialog_get_dialog(card->dialog);
  dialog_set_text(dialog, card->text);
  dialog_set_background_color(dialog, ACTIONS_ACCENT);
  dialog_set_text_color(dialog, GColorWhite);
  dialog_set_icon(dialog, RESOURCE_ID_ALARM_CLOCK_LARGE);
  const DialogCallbacks callbacks = {.unload = prv_timer_card_unload};
  dialog_set_callbacks(dialog, &callbacks, card);
  simple_dialog_push(card->dialog, modal_manager_get_window_stack(ModalPriorityAlert));

  light_enable_interaction();
  vibes_long_pulse();
  card->buzz_timer = evented_timer_register(TIMER_BUZZ_INTERVAL_MS, true, prv_timer_buzz_cb, card);
}

// ---- a notice ----

//! How long a notice stays up if nobody dismisses it.
#define NOTICE_TIMEOUT_MS (15 * 1000)

typedef struct {
  SimpleDialog *dialog;
  char text[AUDIO_COMPANION_WATCH_ACTION_NOTICE_MAX_BYTES + 1];
} NoticeCard;

static void prv_notice_unload(void *context) {
  task_free(context);
}

static void prv_notice_handler(const char *text) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  NoticeCard *card = task_zalloc_check(sizeof(*card));
  strncpy(card->text, text, sizeof(card->text) - 1);
  card->dialog = simple_dialog_create("Muse Notice");
  Dialog *dialog = simple_dialog_get_dialog(card->dialog);
  dialog_set_text(dialog, card->text);
  dialog_set_background_color(dialog, ACTIONS_ACCENT);
  dialog_set_text_color(dialog, GColorWhite);
  dialog_set_icon(dialog, RESOURCE_ID_GENERIC_WARNING_LARGE);
  dialog_set_timeout(dialog, NOTICE_TIMEOUT_MS);
  const DialogCallbacks callbacks = {.unload = prv_notice_unload};
  dialog_set_callbacks(dialog, &callbacks, card);
  simple_dialog_push(card->dialog, modal_manager_get_window_stack(ModalPriorityGeneric));
  light_enable_interaction();
  vibes_short_pulse();
}

// ---- a choice ----

typedef struct {
  Window window;
  MenuLayer menu;
  uint8_t action_id;
  bool answered;
  EventedTimerID timeout_timer;
  WatchActionRequest request;
} ChoiceWindow;

static ChoiceWindow *s_choice;

static GFont prv_question_font(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
}

static GFont prv_option_font(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
}

static int16_t prv_text_height(const char *text, GFont font, int16_t width) {
  const GSize size = graphics_text_layout_get_max_used_size(
      graphics_context_get_current_context(), text, font, GRect(0, 0, width, 1000),
      GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  return size.h;
}

static void prv_choice_finish(ChoiceWindow *choice, uint8_t status, uint8_t index) {
  if (choice->answered) {
    return;
  }
  choice->answered = true;
  watch_actions_choice_result(choice->action_id, status, index);
  window_stack_remove(&choice->window, true);
}

static uint16_t prv_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  ChoiceWindow *choice = context;
  return choice->request.choose.count;
}

static int16_t prv_header_height(MenuLayer *menu, uint16_t section, void *context) {
  ChoiceWindow *choice = context;
  const int16_t width =
      layer_get_bounds_by_value(menu_layer_get_layer(menu)).size.w - 2 * ACTIONS_MARGIN;
  return prv_text_height(choice->request.choose.question, prv_question_font(), width) +
         2 * ACTIONS_MARGIN;
}

static void prv_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *context) {
  ChoiceWindow *choice = context;
  GRect box = layer_get_bounds_by_value(cell);
  box = grect_inset(box, GEdgeInsets(ACTIONS_MARGIN / 2, ACTIONS_MARGIN, 0, ACTIONS_MARGIN));
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, choice->request.choose.question, prv_question_font(), box,
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
}

static int16_t prv_cell_height(MenuLayer *menu, MenuIndex *index, void *context) {
  ChoiceWindow *choice = context;
  const int16_t width =
      layer_get_bounds_by_value(menu_layer_get_layer(menu)).size.w - 2 * ACTIONS_MARGIN;
  const int16_t text = prv_text_height(choice->request.choose.options[index->row],
                                       prv_option_font(), width);
  const int16_t min = menu_cell_basic_cell_height();
  return MAX(min, text + ACTIONS_MARGIN + 4);
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  ChoiceWindow *choice = context;
  const char *option = choice->request.choose.options[index->row];
  GRect box = layer_get_bounds_by_value(cell);
  const int16_t width = box.size.w - 2 * ACTIONS_MARGIN;
  const int16_t text = prv_text_height(option, prv_option_font(), width);
  box = GRect(ACTIONS_MARGIN, (box.size.h - text) / 2 - 4, width, text + 4);
  graphics_context_set_text_color(
      ctx, menu_cell_layer_is_highlighted(cell) ? GColorWhite : GColorBlack);
  graphics_draw_text(ctx, option, prv_option_font(), box, GTextOverflowModeWordWrap,
                     GTextAlignmentLeft, NULL);
}

static void prv_select(MenuLayer *menu, MenuIndex *index, void *context) {
  prv_choice_finish(context, AudioCompanionWatchActionStatusDone, (uint8_t)index->row);
}

static void prv_choice_timeout_cb(void *data) {
  ChoiceWindow *choice = data;
  choice->timeout_timer = EVENTED_TIMER_INVALID_ID;
  prv_choice_finish(choice, AudioCompanionWatchActionStatusTimedOut, 0);
}

static void prv_choice_unload(Window *window) {
  ChoiceWindow *choice = window_get_user_data(window);
  if (!choice->answered) {
    // Back, or taken off the screen some other way: the wearer didn't pick.
    choice->answered = true;
    watch_actions_choice_result(choice->action_id, AudioCompanionWatchActionStatusDeclined, 0);
  }
  if (choice->timeout_timer != EVENTED_TIMER_INVALID_ID) {
    evented_timer_cancel(choice->timeout_timer);
  }
  menu_layer_deinit(&choice->menu);
  window_deinit(&choice->window);
  if (s_choice == choice) {
    s_choice = NULL;
  }
  kernel_free(choice);
}

static void prv_choose_handler(uint8_t action_id, const WatchActionRequest *request) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  if (s_choice) {
    watch_actions_choice_result(action_id, AudioCompanionWatchActionStatusFailed, 0);
    return;
  }
  ChoiceWindow *choice = kernel_zalloc(sizeof(*choice));
  if (!choice) {
    watch_actions_choice_result(action_id, AudioCompanionWatchActionStatusFailed, 0);
    return;
  }
  choice->action_id = action_id;
  choice->request = *request;

  window_init(&choice->window, WINDOW_NAME("Muse Choice"));
  window_set_user_data(&choice->window, choice);
  window_set_window_handlers(&choice->window, &(WindowHandlers){.unload = prv_choice_unload});
  Layer *root = window_get_root_layer(&choice->window);
  menu_layer_init(&choice->menu, &root->bounds);
  menu_layer_set_callbacks(&choice->menu, choice, &(MenuLayerCallbacks){
    .get_num_rows = prv_num_rows,
    .get_header_height = prv_header_height,
    .draw_header = prv_draw_header,
    .get_cell_height = prv_cell_height,
    .draw_row = prv_draw_row,
    .select_click = prv_select,
  });
  menu_layer_set_highlight_colors(&choice->menu, ACTIONS_ACCENT, GColorWhite);
  menu_layer_set_click_config_onto_window(&choice->menu, &choice->window);
  layer_add_child(root, menu_layer_get_layer(&choice->menu));

  s_choice = choice;
  window_stack_push(modal_manager_get_window_stack(ModalPriorityGeneric), &choice->window, true);
  choice->timeout_timer = evented_timer_register(request->choose.timeout_s * 1000, false,
                                                 prv_choice_timeout_cb, choice);
  light_enable_interaction();
  vibes_short_pulse();
}

static void prv_dismiss_handler(void) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  if (!s_choice) {
    return;
  }
  // The question's asker is gone: nothing to answer, nothing to send.
  s_choice->answered = true;
  window_stack_remove(&s_choice->window, true);
}


// ---- an answer that arrived while Muse was closed ----

//! The answer (up to 1536 bytes) and its options as a list.
#define MUSE_NOTIFICATION_BODY_BYTES                                                              \
  (AUDIO_COMPANION_ASSISTANT_ANSWER_BYTES +                                                       \
   AUDIO_COMPANION_ASSISTANT_MAX_OPTIONS * (AUDIO_COMPANION_ASSISTANT_OPTION_BYTES + 4) + 4)

static void *prv_notification_build(const AudioCompanionAssistantView *view) {
  char *body = kernel_malloc(MUSE_NOTIFICATION_BODY_BYTES);
  if (!body) {
    return NULL;
  }
  assistant_view_notification_body(view, body, MUSE_NOTIFICATION_BODY_BYTES);
  const bool failed = view->phase == AudioCompanionAssistantPhaseError;

  AttributeList attrs = {0};
  attribute_list_add_cstring(&attrs, AttributeIdTitle, "Muse");
  if (view->heard[0]) {
    // What was asked, above what Muse said about it.
    attribute_list_add_cstring(&attrs, AttributeIdSubtitle, view->heard);
  }
  attribute_list_add_cstring(&attrs, AttributeIdBody, body);
  attribute_list_add_uint32(&attrs, AttributeIdIconTiny, TIMELINE_RESOURCE_GENERIC_QUESTION);
  attribute_list_add_uint8(&attrs, AttributeIdBgColor,
                           PBL_IF_COLOR_ELSE(GColorCobaltBlueARGB8, GColorDarkGrayARGB8));

  // Muse's options first (picking one answers it), then back into the conversation, then away.
  const uint8_t options = failed ? 0 : view->option_count;
  AttributeList option_attrs[AUDIO_COMPANION_ASSISTANT_MAX_OPTIONS] = {0};
  AttributeList open_attrs = {0};
  AttributeList dismiss_attrs = {0};
  TimelineItemAction actions[AUDIO_COMPANION_ASSISTANT_MAX_OPTIONS + 2];
  uint8_t count = 0;
  for (uint8_t i = 0; i < options; i++) {
    if (!view->options[i][0]) {
      continue;
    }
    attribute_list_add_cstring(&option_attrs[i], AttributeIdTitle, view->options[i]);
    attribute_list_add_uint32(&option_attrs[i], AttributeIdLaunchCode,
                              AUDIO_COMPANION_ASSISTANT_LAUNCH_PICK_CODE(view->turn, i));
    actions[count] = (TimelineItemAction){
      .id = count, .type = TimelineItemActionTypeOpenWatchApp, .attr_list = option_attrs[i]};
    count++;
  }
  attribute_list_add_cstring(&open_attrs, AttributeIdTitle, failed ? "Try Again" : "Reply");
  attribute_list_add_uint32(&open_attrs, AttributeIdLaunchCode,
                            AUDIO_COMPANION_ASSISTANT_LAUNCH_OPEN_CODE(view->turn));
  actions[count] = (TimelineItemAction){
    .id = count, .type = TimelineItemActionTypeOpenWatchApp, .attr_list = open_attrs};
  count++;
  attribute_list_add_cstring(&dismiss_attrs, AttributeIdTitle, "Dismiss");
  actions[count] = (TimelineItemAction){
    .id = count, .type = TimelineItemActionTypeDismiss, .attr_list = dismiss_attrs};
  count++;

  TimelineItemActionGroup group = {.num_actions = count, .actions = actions};
  TimelineItem *item = timeline_item_create_with_attributes(
      rtc_get_time(), 0, TimelineItemTypeNotification, LayoutIdNotification, &attrs, &group);
  if (item) {
    item->header.from_watch = true;
    item->header.parent_id = (Uuid)MUSE_APP_UUID;
  }
  for (uint8_t i = 0; i < AUDIO_COMPANION_ASSISTANT_MAX_OPTIONS; i++) {
    attribute_list_destroy_list(&option_attrs[i]);
  }
  attribute_list_destroy_list(&open_attrs);
  attribute_list_destroy_list(&dismiss_attrs);
  attribute_list_destroy_list(&attrs);
  kernel_free(body);
  return item;
}

static void prv_notification_post(void *built, Uuid *id_out) {
  TimelineItem *item = built;
  *id_out = item->header.id;
  // An ordinary notification, so Quiet Time, the filter, vibes and the list all apply.
  notifications_add_notification(item);
  timeline_item_destroy(item);
}

static void prv_notification_remove(const Uuid *id) {
  if (!notification_storage_notification_exists(id)) {
    return;
  }
  notification_storage_remove(id);
  Uuid removed = *id;
  notifications_handle_notification_removed(&removed);
}

bool audio_companion_assistant_open_from_notification(const TimelineItem *item) {
  static const Uuid muse = MUSE_APP_UUID;
  if (!item || !uuid_equal(&item->header.parent_id, &muse)) {
    return false;
  }
  for (uint8_t i = 0; i < item->action_group.num_actions; i++) {
    const TimelineItemAction *action = &item->action_group.actions[i];
    if (action->type != TimelineItemActionTypeOpenWatchApp) {
      continue;
    }
    const uint32_t code = attribute_get_uint32(&action->attr_list, AttributeIdLaunchCode, 0);
    if ((code & AUDIO_COMPANION_ASSISTANT_LAUNCH_OPEN_AT) ||
        code == AUDIO_COMPANION_ASSISTANT_LAUNCH_OPEN) {
      // Back into the conversation, at this exchange, as if Muse had never been left.
      timeline_invoke_action(item, action, NULL);
      return true;
    }
  }
  return false;
}

bool audio_companion_assistant_open_notification(const Uuid *id) {
  TimelineItem item;
  if (!id || !notification_storage_get(id, &item)) {
    return false;
  }
  const bool opened = audio_companion_assistant_open_from_notification(&item);
  timeline_item_free_allocated_buffer(&item);
  return opened;
}

static const AudioCompanionAssistantNotifier s_notifier = {
  .build = prv_notification_build,
  .post = prv_notification_post,
  .remove = prv_notification_remove,
};

void audio_companion_actions_ui_init(void) {
  watch_actions_set_ui_handlers(prv_choose_handler, prv_timer_done_handler, prv_dismiss_handler);
  watch_actions_set_notice_handler(prv_notice_handler);
  audio_companion_assistant_set_notifier(&s_notifier);
}

#else

void audio_companion_actions_ui_init(void) {
}

bool audio_companion_assistant_open_from_notification(const TimelineItem *item) {
  return false;
}

bool audio_companion_assistant_open_notification(const Uuid *id) {
  return false;
}

#endif
