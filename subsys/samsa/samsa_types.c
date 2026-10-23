/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// Firmware types that apps read and write directly, laid out as the 32-bit
// firmware of a watch lays them out.

#include "samsa_runtime.h"

#include <applib/graphics/gtypes.h>
#include <applib/legacy2/ui/animation_legacy2.h>
#include <applib/legacy2/ui/property_animation_legacy2.h>

// GBitmap on a watch: addr, row_size_bytes, info_flags, bounds, palette and
// padding.
enum {
  GBITMAP_ADDR = 0,
  GBITMAP_ROW_SIZE = 4,
  GBITMAP_INFO = 6,
  GBITMAP_BOUNDS = 8,
  GBITMAP_PALETTE = 16,
  GBITMAP_PADDING = 20,
  GBITMAP_SIZE = 24,
};

static void prv_gbitmap_g2h(const uint8_t *g, void *h) {
  GBitmap *b = h;
  b->addr = samsa_g2h(samsa_rd32(g + GBITMAP_ADDR));
  memcpy(&b->row_size_bytes, g + GBITMAP_ROW_SIZE, sizeof(b->row_size_bytes));
  memcpy(&b->info_flags, g + GBITMAP_INFO, sizeof(b->info_flags));
  memcpy(&b->bounds, g + GBITMAP_BOUNDS, sizeof(b->bounds));
  b->palette = samsa_g2h(samsa_rd32(g + GBITMAP_PALETTE));
  b->padding = (int32_t)samsa_rd32(g + GBITMAP_PADDING);
}

static void prv_gbitmap_h2g(const void *h, uint8_t *g) {
  const GBitmap *b = h;
  samsa_wr32(g + GBITMAP_ADDR, samsa_h2g(b->addr));
  memcpy(g + GBITMAP_ROW_SIZE, &b->row_size_bytes, sizeof(b->row_size_bytes));
  memcpy(g + GBITMAP_INFO, &b->info_flags, sizeof(b->info_flags));
  memcpy(g + GBITMAP_BOUNDS, &b->bounds, sizeof(b->bounds));
  samsa_wr32(g + GBITMAP_PALETTE, samsa_h2g(b->palette));
  samsa_wr32(g + GBITMAP_PADDING, (uint32_t)b->padding);
}

const SamsaType g_samsa_gbitmap_type = {
  .name = "GBitmap",
  .guest_size = GBITMAP_SIZE,
  .host_size = sizeof(GBitmap),
  .g2h = prv_gbitmap_g2h,
  .h2g = prv_gbitmap_h2g,
};

// SDK 2 published its animation structures, and apps use them: some own
// their animations and drive them with the update functions, others write the
// fields of the ones the firmware created, its values most of all. An app's
// own converts in full; the app's view of the firmware's converts back only
// the fields an app may write.
enum {
  ANIMATION_LIST_NODE = 0,
  ANIMATION_IMPLEMENTATION = 8,
  ANIMATION_STARTED = 12,
  ANIMATION_STOPPED = 16,
  ANIMATION_CONTEXT = 20,
  ANIMATION_START_TIME = 24,
  ANIMATION_DELAY = 28,
  ANIMATION_DURATION = 32,
  ANIMATION_FLAGS = 36,
  ANIMATION_SIZE = 40,
  PROPERTY_ANIMATION_TO = 40,
  PROPERTY_ANIMATION_FROM = 48,
  PROPERTY_ANIMATION_SUBJECT = 56,
  PROPERTY_ANIMATION_SIZE = 60,
};

static void prv_animation_legacy2_shadow_g2h(const uint8_t *g, void *h) {
  AnimationLegacy2 *a = h;
  a->delay_ms = samsa_rd32(g + ANIMATION_DELAY);
  a->duration_ms = samsa_rd32(g + ANIMATION_DURATION);
}

static void prv_animation_legacy2_convert(const uint8_t *g, AnimationLegacy2 *a,
                                          const SamsaType *implementation) {
  a->list_node.next = samsa_g2h(samsa_rd32(g + ANIMATION_LIST_NODE));
  a->list_node.prev = samsa_g2h(samsa_rd32(g + ANIMATION_LIST_NODE + 4));
  a->implementation = samsa_obj_in_soft(samsa_rd32(g + ANIMATION_IMPLEMENTATION), implementation);
  a->handlers.started =
      samsa_cb_get_AnimationLegacy2StartedHandler(samsa_rd32(g + ANIMATION_STARTED));
  a->handlers.stopped =
      samsa_cb_get_AnimationLegacy2StoppedHandler(samsa_rd32(g + ANIMATION_STOPPED));
  a->context = samsa_g2h(samsa_rd32(g + ANIMATION_CONTEXT));
  a->abs_start_time_ms = samsa_rd32(g + ANIMATION_START_TIME);
  prv_animation_legacy2_shadow_g2h(g, a);
  const uint32_t flags = samsa_rd32(g + ANIMATION_FLAGS);
  a->curve = flags & 7;
  a->is_completed = (flags >> 3) & 1;
  a->custom_curve_function = flags >> 4;
}

static void prv_animation_legacy2_g2h(const uint8_t *g, void *h) {
  prv_animation_legacy2_convert(g, h, g_samsa_type_AnimationLegacy2Implementation);
}

static void prv_animation_legacy2_h2g(const void *h, uint8_t *g) {
  const AnimationLegacy2 *a = h;
  samsa_wr32(g + ANIMATION_LIST_NODE, samsa_h2g(a->list_node.next));
  samsa_wr32(g + ANIMATION_LIST_NODE + 4, samsa_h2g(a->list_node.prev));
  samsa_wr32(g + ANIMATION_IMPLEMENTATION, samsa_obj_guest_of((void *)a->implementation, nullptr));
  samsa_wr32(g + ANIMATION_STARTED, samsa_cb_to_guest((void *)a->handlers.started));
  samsa_wr32(g + ANIMATION_STOPPED, samsa_cb_to_guest((void *)a->handlers.stopped));
  samsa_wr32(g + ANIMATION_CONTEXT, samsa_h2g(a->context));
  samsa_wr32(g + ANIMATION_START_TIME, a->abs_start_time_ms);
  samsa_wr32(g + ANIMATION_DELAY, a->delay_ms);
  samsa_wr32(g + ANIMATION_DURATION, a->duration_ms);
  samsa_wr32(g + ANIMATION_FLAGS, (uint32_t)a->curve | (uint32_t)a->is_completed << 3 |
                                      (uint32_t)a->custom_curve_function << 4);
}

const SamsaType g_samsa_animation_legacy2_type = {
  .name = "AnimationLegacy2",
  .guest_size = ANIMATION_SIZE,
  .host_size = sizeof(AnimationLegacy2),
  .g2h = prv_animation_legacy2_g2h,
  .h2g = prv_animation_legacy2_h2g,
  .shadow_g2h = prv_animation_legacy2_shadow_g2h,
};

static void prv_property_animation_legacy2_values(const uint8_t *g, PropertyAnimationLegacy2 *p) {
  memcpy(&p->values.to, g + PROPERTY_ANIMATION_TO, sizeof(p->values.to));
  memcpy(&p->values.from, g + PROPERTY_ANIMATION_FROM, sizeof(p->values.from));
  p->subject = samsa_g2h(samsa_rd32(g + PROPERTY_ANIMATION_SUBJECT));
}

static void prv_property_animation_legacy2_shadow_g2h(const uint8_t *g, void *h) {
  PropertyAnimationLegacy2 *p = h;
  prv_animation_legacy2_shadow_g2h(g, &p->animation);
  prv_property_animation_legacy2_values(g, p);
}

static void prv_property_animation_legacy2_g2h(const uint8_t *g, void *h) {
  PropertyAnimationLegacy2 *p = h;
  prv_animation_legacy2_convert(g, &p->animation,
                                g_samsa_type_PropertyAnimationLegacy2Implementation);
  prv_property_animation_legacy2_values(g, p);
}

static void prv_property_animation_legacy2_h2g(const void *h, uint8_t *g) {
  const PropertyAnimationLegacy2 *p = h;
  prv_animation_legacy2_h2g(&p->animation, g);
  memcpy(g + PROPERTY_ANIMATION_TO, &p->values.to, sizeof(p->values.to));
  memcpy(g + PROPERTY_ANIMATION_FROM, &p->values.from, sizeof(p->values.from));
  samsa_wr32(g + PROPERTY_ANIMATION_SUBJECT, samsa_h2g(p->subject));
}

const SamsaType g_samsa_property_animation_legacy2_type = {
  .name = "PropertyAnimationLegacy2",
  .guest_size = PROPERTY_ANIMATION_SIZE,
  .host_size = sizeof(PropertyAnimationLegacy2),
  .g2h = prv_property_animation_legacy2_g2h,
  .h2g = prv_property_animation_legacy2_h2g,
  .shadow_g2h = prv_property_animation_legacy2_shadow_g2h,
};
