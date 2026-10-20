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
#include "kernel/pbl_malloc.h"
#include "kernel/pebble_tasks.h"
#include "kernel/ui/kernel_ui.h"
#include "kernel/ui/modals/modal_manager.h"
#include "pbl/services/evented_timer.h"
#include "pbl/services/light.h"
#include "resource/resource_ids.auto.h"
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

void audio_companion_actions_ui_init(void) {
  watch_actions_set_ui_handlers(prv_choose_handler, prv_timer_done_handler, prv_dismiss_handler);
  watch_actions_set_notice_handler(prv_notice_handler);
}

#else

void audio_companion_actions_ui_init(void) {
}

#endif
