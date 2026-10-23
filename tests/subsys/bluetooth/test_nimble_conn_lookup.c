/* SPDX-License-Identifier: Apache-2.0 */

#include "nimble_type_conversions.h"

#include <string.h>

#include <clar.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>

// Stubs
///////////////////////////////////////////////////////////

#include <stubs_logging.h>
#include <stubs_nimble_npl.h>

// Fakes: up to two links. NimBLE's ble_gap_conn_find_by_addr() returns the newest link to an
// address, so with the same iPhone on the phone link and the Muse setup window's private link,
// a plain lookup lands on the private one.
///////////////////////////////////////////////////////////

#define PHONE_HANDLE   (1)
#define PRIVATE_HANDLE (2)

static const ble_addr_t s_phone_id = {.type = BLE_ADDR_PUBLIC, .val = {1, 2, 3, 4, 5, 6}};
static const ble_addr_t s_other_id = {.type = BLE_ADDR_PUBLIC, .val = {9, 9, 9, 9, 9, 9}};

typedef struct {
  bool up;
  ble_addr_t peer;
} FakeLink;

static FakeLink s_links[3];

int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out_desc) {
  if (handle >= 3 || !s_links[handle].up) {
    return BLE_HS_ENOTCONN;
  }
  memset(out_desc, 0, sizeof(*out_desc));
  out_desc->conn_handle = handle;
  out_desc->peer_id_addr = s_links[handle].peer;
  out_desc->peer_ota_addr = s_links[handle].peer;
  return 0;
}

int ble_gap_conn_find_by_addr(const ble_addr_t *addr, struct ble_gap_conn_desc *out_desc) {
  for (int handle = 2; handle >= 1; handle--) {  // newest first
    if (s_links[handle].up && ble_addr_cmp(addr, &s_links[handle].peer) == 0) {
      return ble_gap_conn_find((uint16_t)handle, out_desc);
    }
  }
  return BLE_HS_ENOTCONN;
}

// Helpers
///////////////////////////////////////////////////////////

static struct pbl_bt_device_internal prv_device(const ble_addr_t *addr) {
  struct pbl_bt_device_internal device;
  nimble_addr_to_pebble_device(addr, &device);
  return device;
}

static void prv_link(uint16_t handle, const ble_addr_t *peer) {
  s_links[handle] = (FakeLink){.up = true, .peer = *peer};
}

// Tests
///////////////////////////////////////////////////////////

void test_nimble_conn_lookup__initialize(void) {
  memset(s_links, 0, sizeof(s_links));
  nimble_conn_set_private_link(BLE_HS_CONN_HANDLE_NONE);
  nimble_conn_clear_phone_link(nimble_conn_phone_link());
}

void test_nimble_conn_lookup__phone_link_alone(void) {
  prv_link(PHONE_HANDLE, &s_phone_id);
  nimble_conn_set_phone_link(PHONE_HANDLE);
  const struct pbl_bt_device_internal device = prv_device(&s_phone_id);
  uint16_t handle = 0;
  cl_assert(pebble_device_to_nimble_conn_handle(&device, &handle));
  cl_assert_equal_i(handle, PHONE_HANDLE);
}

void test_nimble_conn_lookup__private_link_never_captures_phone_traffic(void) {
  prv_link(PHONE_HANDLE, &s_phone_id);
  nimble_conn_set_phone_link(PHONE_HANDLE);
  prv_link(PRIVATE_HANDLE, &s_phone_id);
  nimble_conn_set_private_link(PRIVATE_HANDLE);

  const struct pbl_bt_device_internal device = prv_device(&s_phone_id);
  uint16_t handle = 0;
  cl_assert(pebble_device_to_nimble_conn_handle(&device, &handle));
  cl_assert_equal_i(handle, PHONE_HANDLE);
}

void test_nimble_conn_lookup__private_link_without_phone_link_finds_nothing(void) {
  prv_link(PRIVATE_HANDLE, &s_phone_id);
  nimble_conn_set_private_link(PRIVATE_HANDLE);

  const struct pbl_bt_device_internal device = prv_device(&s_phone_id);
  uint16_t handle = 0;
  cl_assert(!pebble_device_to_nimble_conn_handle(&device, &handle));
}

void test_nimble_conn_lookup__phone_link_to_another_peer_is_not_substituted(void) {
  prv_link(PHONE_HANDLE, &s_other_id);
  nimble_conn_set_phone_link(PHONE_HANDLE);
  prv_link(PRIVATE_HANDLE, &s_phone_id);
  nimble_conn_set_private_link(PRIVATE_HANDLE);

  const struct pbl_bt_device_internal device = prv_device(&s_phone_id);
  uint16_t handle = 0;
  cl_assert(!pebble_device_to_nimble_conn_handle(&device, &handle));
}

void test_nimble_conn_lookup__phone_link_cleared_only_by_its_own_disconnect(void) {
  nimble_conn_set_phone_link(PHONE_HANDLE);
  nimble_conn_clear_phone_link(PRIVATE_HANDLE);
  cl_assert_equal_i(nimble_conn_phone_link(), PHONE_HANDLE);
  nimble_conn_clear_phone_link(PHONE_HANDLE);
  cl_assert_equal_i(nimble_conn_phone_link(), BLE_HS_CONN_HANDLE_NONE);
}

void test_nimble_conn_lookup__private_flag(void) {
  cl_assert(!nimble_conn_is_private_link(BLE_HS_CONN_HANDLE_NONE));
  nimble_conn_set_private_link(PRIVATE_HANDLE);
  cl_assert(nimble_conn_is_private_link(PRIVATE_HANDLE));
  cl_assert(!nimble_conn_is_private_link(PHONE_HANDLE));
  nimble_conn_set_private_link(BLE_HS_CONN_HANDLE_NONE);
  cl_assert(!nimble_conn_is_private_link(PRIVATE_HANDLE));
}
