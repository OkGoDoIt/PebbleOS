/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <samsa/samsa.h>

//! Where in a process's address space the SDK's functions are: a branch there
//! stops the interpreter with the thunk to run.
#define SAMSA_GUEST_HOOKS UINT32_C(0x0f000000)

typedef void (*SamsaThunk)(SamsaCpu *cpu);

//! The assembler symbol of a C function name, for declaring one under
//! another name.
#define SAMSA_STR(x)       #x
#define SAMSA_XSTR(x)      SAMSA_STR(x)
#define SAMSA_SYMBOL(name) SAMSA_XSTR(__USER_LABEL_PREFIX__) name

//! A type the app and the host lay out differently.
typedef struct {
  const char *name;
  uint32_t guest_size;
  size_t host_size;
  void (*g2h)(const uint8_t *guest, void *host);
  void (*h2g)(const void *host, uint8_t *guest);
  //! For the app's view of a firmware object, if it must not convert all
  //! of it back: the fields the app may write.
  void (*shadow_g2h)(const uint8_t *guest, void *host);
} SamsaType;

//! Host trampolines for app callbacks of one signature.
typedef struct {
  const char *signature;
  uint32_t *guest;
  void *const *fns;
  unsigned count;
} SamsaCbPool;

static inline uint32_t samsa_rd32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static inline int32_t samsa_rd32s(const uint8_t *p) {
  return (int32_t)samsa_rd32(p);
}

static inline void samsa_wr32(uint8_t *p, uint32_t v) {
  memcpy(p, &v, sizeof(v));
}

// Addresses are those of the process running on the current task, the app's
// or the worker's.

//! Host pointer for an app address. Values that are not addresses (an int
//! smuggled through a void * context) pass through unchanged.
void *samsa_g2h(uint32_t guest);
//! Host pointer for an app address that has to be one; kills the app if not.
void *samsa_g2h_ptr(uint32_t guest);
//! Host pointer for an app pointer to an opaque firmware object, which may
//! also be a handle the firmware gave out; kills the app if it is neither.
void *samsa_g2h_handle(uint32_t guest);
//! App address for a host pointer.
uint32_t samsa_h2g(const void *host);

//! Host object for an app pointer to a type laid out differently.
void *samsa_obj_in(uint32_t guest, const SamsaType *type);
//! Host array for an app pointer to count objects of a type laid out differently.
void *samsa_obj_in_array(uint32_t guest, uint32_t count, const SamsaType *type);
//! As samsa_obj_in_array(), for an array the firmware keeps and reads later:
//! it is converted again before each SDK call if the app changed it.
void *samsa_obj_in_array_retained(uint32_t guest, uint32_t count, const SamsaType *type);
//! As samsa_obj_in_array(), but NULL rather than a dead app for a bad pointer.
void *samsa_obj_in_array_soft(uint32_t guest, uint32_t count, const SamsaType *type);
//! App address of the object a host copy was made from, of any type if NULL.
uint32_t samsa_obj_guest_of(void *host, const SamsaType *type);
//! As samsa_obj_in(), but NULL rather than a dead app for a bad pointer.
void *samsa_obj_in_soft(uint32_t guest, const SamsaType *type);
//! App address of a host object of a type laid out differently.
uint32_t samsa_obj_out(void *host, const SamsaType *type);
//! Copy a host object back to the app after the firmware changed it.
void samsa_obj_sync(uint32_t guest, void *host, const SamsaType *type);
//! Copy an app object back to the host after a callback changed it.
void samsa_obj_back(uint32_t guest, void *host, const SamsaType *type);

void *samsa_cb_get(SamsaCbPool *pool, uint32_t guest_fn);
//! A trampoline for a callback of any signature taking a pointer and one value.
void *samsa_cb_raw_get(uint32_t guest_fn);
uint32_t samsa_cb_to_guest(void *host_fn);

//! Call an app function; kills the app if it faults.
void samsa_call_guest(uint32_t fn, const uint32_t *args, unsigned nargs, uint32_t *ret);

//! Generated converters the hand-written types use.
extern const SamsaType *const g_samsa_type_AnimationLegacy2Implementation;
extern const SamsaType *const g_samsa_type_PropertyAnimationLegacy2Implementation;
void *samsa_cb_get_AnimationLegacy2StartedHandler(uint32_t guest_fn);
void *samsa_cb_get_AnimationLegacy2StoppedHandler(uint32_t guest_fn);

//! Firmware types as apps that read them directly expect them.
extern const SamsaType g_samsa_gbitmap_type;
extern const SamsaType g_samsa_animation_legacy2_type;
extern const SamsaType g_samsa_property_animation_legacy2_type;

extern const SamsaThunk g_samsa_thunks[];
//! The SDK's function table as processes see it: the hook of each function.
extern const uint32_t g_samsa_sdk_table[];
extern const char *const g_samsa_thunk_names[];
extern const unsigned g_samsa_num_thunks;
extern SamsaCbPool *const g_samsa_cb_pools[];
