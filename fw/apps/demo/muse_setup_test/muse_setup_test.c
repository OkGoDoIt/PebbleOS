/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>

#include <pbl/bluetooth/muse_setup.h>
#include <pbl/drivers/rtc.h>
#include <pbl/services/system_task.h>

#include <applib/app.h>
#include <applib/app_timer.h>
#include <applib/fonts/fonts.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/text_layer.h>
#include <applib/ui/window.h>
#include <kernel/pbl_malloc.h>
#include <process_management/pebble_process_md.h>
#include <process_state/app_state/app_state.h>

//! The test window lasts as long as a real pairing window would.
#define MUSE_SETUP_TEST_WINDOW_MS (10 * 60 * 1000)

typedef struct {
  Window *window;
  TextLayer *text;
  AppTimer *timer;
  char suffix[PBL_BT_MUSE_SETUP_SUFFIX_LEN + 1];
  char buffer[400];
} MuseSetupTestData;

static void prv_open_cb(void *context) {
  const char *suffix = context;
  pbl_bt_muse_setup_open(suffix, MUSE_SETUP_TEST_WINDOW_MS);
}

static void prv_close_cb(void *context) {
  pbl_bt_muse_setup_close();
}

static void prv_refresh(void *context) {
  MuseSetupTestData *data = app_state_get_user_data();
  PblBtMuseSetupStatus status;
  pbl_bt_muse_setup_get_status(&status);

  const char *link = status.connected ? "connected" : status.advertising ? "advertising" : "-";
  const char *same = !status.connected ? "-"
                     : !status.same_phone_known ? "?"
                     : status.same_phone ? "yes" : "NO";
  snprintf(data->buffer, sizeof(data->buffer),
           "Muse setup %s\n%s\n%lu:%02lu left\nLink: %s\nSame phone: %s%s\nMTU %u sub %s\n"
           "RX %lu (%lu B)\nTX %lu  refused %lu\nPhone link: %s\nerr %d  ev %d (x%lu)\n%s%s\n"
           "SELECT: %s",
           status.open ? "OPEN" : "closed", status.name[0] ? status.name : "(not opened)",
           (unsigned long)(status.seconds_left / 60), (unsigned long)(status.seconds_left % 60),
           link, same, status.connected && status.resolved ? " (rpa)" : "", status.mtu,
           status.subscribed ? "yes" : "no", (unsigned long)status.rx_packets,
           (unsigned long)status.rx_bytes, (unsigned long)status.tx_packets,
           (unsigned long)status.tx_refused, status.phone_link_up ? "up" : "DOWN",
           status.last_error, status.last_gap_event, (unsigned long)status.gap_events,
           status.close_reason[0] ? "closed: " : "", status.close_reason,
           status.open ? "close" : "open");
  text_layer_set_text(data->text, data->buffer);
  data->timer = app_timer_register(500, prv_refresh, NULL);
}

static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  MuseSetupTestData *data = app_state_get_user_data();
  PblBtMuseSetupStatus status;
  pbl_bt_muse_setup_get_status(&status);
  if (status.open) {
    system_task_add_callback(prv_close_cb, NULL);
  } else {
    // A fresh gadget suffix each time; the real one comes from the companion app's identity.
    const uint32_t seed = (uint32_t)rtc_get_ticks() * 2654435761u;
    snprintf(data->suffix, sizeof(data->suffix), "%06lX", (unsigned long)(seed >> 8) & 0xFFFFFF);
    system_task_add_callback(prv_open_cb, data->suffix);
  }
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
}

static void prv_init(void) {
  MuseSetupTestData *data = app_zalloc_check(sizeof(*data));
  app_state_set_user_data(data);

  data->window = window_create();
  window_set_click_config_provider(data->window, prv_click_config);
  Layer *root = window_get_root_layer(data->window);
  data->text = text_layer_create(root->bounds);
  text_layer_set_font(data->text, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(root, text_layer_get_layer(data->text));
  app_window_stack_push(data->window, true);
  prv_refresh(NULL);
}

static void prv_deinit(void) {
  MuseSetupTestData *data = app_state_get_user_data();
  // Leaving the app closes the window: it must not outlive the screen that shows it.
  system_task_add_callback(prv_close_cb, NULL);
  app_timer_cancel(data->timer);
  text_layer_destroy(data->text);
  window_destroy(data->window);
  app_free(data);
}

static void prv_main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}

const PebbleProcessMd *muse_setup_test_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common =
        {
          .main_func = prv_main,
          // UUID: 6d3f1c2a-9b47-4e15-8a0c-5e2b7d91c4f3
          .uuid = {0x6d, 0x3f, 0x1c, 0x2a, 0x9b, 0x47, 0x4e, 0x15, 0x8a, 0x0c, 0x5e, 0x2b, 0x7d,
                   0x91, 0xc4, 0xf3},
        },
    .name = "Muse Setup Test",
  };
  return (const PebbleProcessMd *)&s_app_info;
}
