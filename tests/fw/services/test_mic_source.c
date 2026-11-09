/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/drivers/audio.h>
#include <pbl/kernel/heap.h>
#include <pbl/services/system_task.h>

#include <clar.h>
#include <fake_mutex.h>
#include <services/mic_manager/mic_source.h>
#include <stubs_logging.h>

static const struct pbl_audio s_device;
const struct pbl_audio *const MIC = &s_device;
static struct pbl_audio_stream s_stream;
static SystemTaskEventCallback s_callbacks[16];
static size_t s_queued;
static size_t s_allocations;
static size_t s_delivered;
static size_t s_released;
static unsigned s_max_free;
static int s_start_result;
static bool s_running;
static int16_t s_frame[320];
static int16_t s_first_samples[32];

void *pbl_malloc(size_t size) {
  void *ptr = malloc(size);
  if (ptr) {
    s_allocations++;
  }
  return ptr;
}

void pbl_free(void *ptr) {
  if (ptr) {
    cl_assert(s_allocations > 0);
    s_allocations--;
  }
  free(ptr);
}

void pbl_heap_stats(struct pbl_heap *heap, unsigned *used, unsigned *free_bytes,
                    unsigned *max_free) {
  *used = 0;
  *free_bytes = *max_free = s_max_free;
}

size_t pbl_audio_ring_bytes(const struct pbl_audio_stream *stream) {
  return stream->period_frames * stream->periods * stream->channels * sizeof(int16_t);
}

void *pbl_audio_buf_alloc(size_t size) {
  return pbl_malloc(size);
}

void pbl_audio_buf_free(void *ptr) {
  pbl_free(ptr);
}

int pbl_audio_start(const struct pbl_audio *audio, const struct pbl_audio_stream *stream) {
  s_stream = *stream;
  s_running = s_start_result == 0;
  return s_start_result;
}

int pbl_audio_stop(const struct pbl_audio *audio) {
  s_running = false;
  return 0;
}

int pbl_audio_period_ready(const struct pbl_audio *audio, void *period) {
  cl_assert(s_running);
  s_released++;
  return 0;
}

bool system_task_add_callback_droppable(SystemTaskEventCallback cb, void *data) {
  cl_assert(s_queued < 16);
  s_callbacks[s_queued++] = cb;
  return true;
}

bool system_task_add_callback_from_isr_droppable(SystemTaskEventCallback cb, void *data) {
  return system_task_add_callback_droppable(cb, data);
}

void system_task_watchdog_feed(void) {
}

static void prv_dispatch_one(void) {
  cl_assert(s_queued > 0);
  SystemTaskEventCallback cb = s_callbacks[0];
  memmove(s_callbacks, s_callbacks + 1, (--s_queued) * sizeof(*s_callbacks));
  cb(nullptr);
}

static void prv_frame(int16_t *samples, size_t count, void *context) {
  cl_assert_equal_i(count, 320);
  cl_assert_equal_p(samples, s_frame);
  cl_assert(s_delivered < 32);
  s_first_samples[s_delivered++] = samples[0];
}

static void prv_period(int16_t value) {
  cl_assert(s_running);
  int16_t *period = s_stream.buf;
  for (size_t i = 0; i < s_stream.period_frames; i++) {
    period[i] = value;
  }
  const struct pbl_audio_pos pos = {};
  s_stream.period_done(MIC, period, &pos, s_stream.arg);
}

void test_mic_source__initialize(void) {
  s_queued = s_delivered = s_released = 0;
  s_max_free = 16384;
  s_start_result = 0;
  memset(s_first_samples, 0, sizeof(s_first_samples));
}

void test_mic_source__cleanup(void) {
  mic_source_stop();
  while (s_queued) {
    prv_dispatch_one();
  }
  cl_assert_equal_i(s_allocations, 0);
  fake_mutex_reset(true);
}

void test_mic_source__batches_dma_without_changing_client_frames(void) {
  cl_assert(mic_source_start(prv_frame, nullptr, s_frame, 320));
  cl_assert_equal_i(s_stream.period_frames, 640);
  cl_assert_equal_i(s_stream.periods, 2);
  prv_period(11);
  prv_period(22);
  cl_assert_equal_i(s_queued, 1);
  cl_assert_equal_i(s_released, 2);
  prv_dispatch_one();
  cl_assert_equal_i(s_delivered, 4);
  cl_assert_equal_i(s_first_samples[0], 11);
  cl_assert_equal_i(s_first_samples[1], 11);
  cl_assert_equal_i(s_first_samples[2], 22);
  cl_assert_equal_i(s_first_samples[3], 22);
  cl_assert_equal_i(s_queued, 0);
}

void test_mic_source__yields_between_bounded_dispatch_batches(void) {
  cl_assert(mic_source_start(prv_frame, nullptr, s_frame, 320));
  for (int i = 0; i < 6; i++) {
    prv_period(i);
  }
  prv_dispatch_one();
  cl_assert_equal_i(s_delivered, 8);
  cl_assert_equal_i(s_queued, 1);
  prv_dispatch_one();
  cl_assert_equal_i(s_delivered, 12);
  cl_assert_equal_i(s_queued, 0);
}

void test_mic_source__queued_work_cannot_deliver_stopped_capture(void) {
  cl_assert(mic_source_start(prv_frame, nullptr, s_frame, 320));
  prv_period(11);
  mic_source_stop();
  prv_dispatch_one();
  cl_assert_equal_i(s_delivered, 0);
  cl_assert_equal_i(s_allocations, 0);
  cl_assert(mic_source_start(prv_frame, nullptr, s_frame, 320));
  prv_period(22);
  prv_dispatch_one();
  cl_assert_equal_i(s_delivered, 2);
  cl_assert_equal_i(s_first_samples[0], 22);

  // An old queued callback may outlive a stop and a new owner starting the source.
  prv_period(11);
  mic_source_stop();
  cl_assert(mic_source_start(prv_frame, nullptr, s_frame, 320));
  prv_period(33);
  while (s_queued) {
    prv_dispatch_one();
  }
  cl_assert_equal_i(s_delivered, 4);
  cl_assert_equal_i(s_first_samples[2], 33);
  cl_assert_equal_i(s_first_samples[3], 33);
}

void test_mic_source__failed_start_and_heap_pressure_leave_capture_retryable(void) {
  s_max_free = 4096 + 2047;
  cl_assert(!mic_source_start(prv_frame, nullptr, s_frame, 320));
  cl_assert_equal_i(s_allocations, 0);
  s_max_free++;
  s_start_result = -EIO;
  cl_assert(!mic_source_start(prv_frame, nullptr, s_frame, 320));
  cl_assert_equal_i(s_allocations, 0);
  s_start_result = 0;
  cl_assert(mic_source_start(prv_frame, nullptr, s_frame, 320));
  prv_period(33);
  prv_dispatch_one();
  cl_assert_equal_i(s_delivered, 2);
}
