/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/audio_companion.h"

#include "auth.h"
#include "reboot_trace.h"
#include "spool.h"

#include "applib/event_service_client.h"
#include "bluetooth/audio_companion_service.h"
#include "comm/bt_conn_mgr.h"
#include "board/board.h"
#include <pbl/drivers/mic.h>
#include <pbl/drivers/rtc.h>
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "pbl/kernel/mutex.h"
#include "pbl/services/battery/battery_state.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/runlevel.h"
#include "pbl/services/system_task.h"
#include "pbl/services/voice/voice_speex.h"
#include "shell/prefs.h"
#include "pbl/logging/logging.h"
#include "system/passert.h"
#include "system/reboot_reason.h"
#include "util/rand.h"
#include "util/time/time.h"

#ifndef UNITTEST
#include "pbl/services/notifications/notifications.h"
#include "pbl/services/timeline/attribute.h"
#include "pbl/services/timeline/item.h"
#endif

#include <inttypes.h>
#include <string.h>

PBL_LOG_MODULE_DEFINE(service_audio_companion, CONFIG_SERVICE_AUDIO_COMPANION_LOG_LEVEL);

#ifndef CONFIG_AUDIO_COMPANION_LOW_BATTERY_PERCENT
#define CONFIG_AUDIO_COMPANION_LOW_BATTERY_PERCENT 20
#endif

#define DRAIN_PERIOD_MS (150)
#define DRAIN_PUSH_THRESHOLD_FRAMES (8)
//! Cap the BLE notifications sent in a single drain callback, and let the repeating drain timer
//! pace the next slice rather than re-posting immediately.
//!
//! This is transport backpressure, not just callback length. Notifications reach the BT
//! controller through a 512-byte SiFli IPC ring. ipc_queue_write() busy-waits with no
//! schedule point while that ring is full; the HCI transport now sleeps instead of spinning
//! (hci_sf32lb52.c:prv_ipc_write), so a full ring can no longer starve KernelBG. The burst
//! cap still matters: sleeping 1 ms per blocked write keeps NimbleHost off the CPU, but a
//! huge offered load still fills the ring and delays every other HCI packet. KernelBG's
//! watchdog bit is fed only at the end of each system-task callback (and by the idle
//! timer, which correctly declines to feed while callbacks are queued), so a callback that
//! cannot run for >6.5 s is still a watchdog reset recorded against KernelBG.
//!
//! Draining a backlog as fast as NimBLE accepts mbufs keeps that ring permanently full.
//! The HCI path now sleeps rather than spinning, so that no longer watchdog-resets the
//! watch, but it still stalls every other HCI packet for the duration. Bound each burst to
//! what the link can absorb inside one drain period: ~4 x (MTU-3) is roughly 6 KB/s, still
//! several times the ~2 KB/s the 16 kHz/20 ms Speex stream produces, so a backlog catches up
//! at a few times real time while leaving the transport idle between slices.
#define DRAIN_MAX_BATCHES_PER_CALL (4)
//! Catch-up ceiling. A backlog that only ever drains at a few times real time never recovers on a
//! busy day, but the hazard above is *sustained* ring saturation, not burst size as such -- and
//! bt_driver_audio_companion_notify_data() returning false means NimBLE would not take another
//! mbuf, which is a different layer than the 512-byte IPC ring. Pace the burst
//! additive-increase / multiplicative-decrease: widen it by DRAIN_BURST_STEP only after a whole
//! slice was accepted while a real backlog remained, and collapse straight back to the base burst
//! on the first refusal. The link never gets more than one over-sized slice before we retreat.
//!
//! 12 x (MTU-3) per 150 ms is ~19 KB/s, ~10x the ~2 KB/s the 16 kHz/20 ms Speex stream produces,
//! and it is reached only after four consecutive clean slices (600 ms). The spool holds at most
//! CONFIG_AUDIO_COMPANION_SPOOL_MAX_BYTES (~98 s of audio), so the deepest possible backlog
//! clears in ~8 s of bursting instead of ~23 s, after which the burst falls back to the base and
//! the radio goes idle between slices again. Steady-state streaming never leaves the base burst,
//! so ordinary airtime and battery are unchanged.
#define DRAIN_MAX_BATCHES_CATCH_UP (12)
#define DRAIN_BURST_STEP (2)
//! Only widen once the backlog is deeper than a base slice can clear, so a single late frame
//! cannot buy extra airtime.
#define DRAIN_BURST_BACKLOG_FRAMES \
  (AUDIO_COMPANION_MAX_FRAMES_PER_DATA_MSG * DRAIN_MAX_BATCHES_PER_CALL)
//! Stop capturing if a streaming session goes this long with no control message from the
//! receiver. The phone checkpoints every ~0.5-2 s while receiving, so a long silence means the
//! app is gone even when the watch never saw a BLE disconnect (e.g. the receiver shares the link
//! with the official app and only dropped its own GATT connection).
#define RECEIVER_LIVENESS_TIMEOUT_MS (15 * 1000)
//! How long capture keeps running for a receiver that is not there before the microphone parks.
//!
//! The spool is drop-oldest, so while offline the watch keeps a rolling window of the most recent
//! audio and reports what it shed as SpoolOverflow gaps. The only reason to stop is battery, and
//! that is a question of minutes, not of the first dropped frame. Ten minutes comfortably outlasts
//! every transient the phone produces — an iOS suspension, a memory jettison and relaunch, a walk
//! out of Bluetooth range and back — while still not running the microphone all night for a phone
//! that has been switched off.
#define OFFLINE_CAPTURE_PARK_MS (10 * 60 * 1000)
//! How long the microphone stays off between listen windows while muted for stationary.
//!
//! The cost of being wrong in this direction is bounded and small — a couple of minutes at the
//! head of a conversation that began while the wrist happened to be still — and the saving is the
//! PDM hardware being off for the other ~97% of the time. Anything much shorter stops being a
//! power saving at all; anything much longer starts losing the beginning of meetings.
#define POWER_SAVE_LISTEN_INTERVAL_MS (2 * 60 * 1000)
//! How long the microphone stays on during a listen window, and how much of that the mute verdict
//! needs to hear.
//!
//! BUG: the window was 5,000 ms and the verdict was `s_silence_suppressing`, which at the default
//! mode needed 5,000 ms of quiet to become true -- so the verdict could never be reached inside
//! the window that existed to collect it, and the stationary mute could not re-engage after its
//! first listen. That mute is the ONLY path that stops mic_start(): suppression saves the encoder
//! and the radio, but the PDM rails, clocks and buffers come down only here. The arithmetic
//! switched off the largest saving the service has.
//!
//! The fix is on the verdict, not the window. Arming windows are now 5-20 s long (see the mode
//! table), and stretching every listen to fit the longest would leave the microphone on for a
//! sixth of each cycle -- most of what the mute was saving. So the listen window stays short and
//! the mute asks the narrower question it actually needs answered: of what this window heard, was
//! the mode's quiet fraction of it quiet? Same threshold, same bar, less evidence, and being
//! wrong costs at most one listen interval of a conversation that the next window picks up.
#define POWER_SAVE_LISTEN_WINDOW_MS (6 * 1000)
//! Frames the verdict must actually have heard, leaving ~1 s of the window for the mic to start.
#define POWER_SAVE_LISTEN_MIN_FRAMES \
  (5000 / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS)
//! How often we re-ask the question while stationary but NOT muted (i.e. the room has voices in
//! it). Only a timer and a bool — the microphone is already on — so this is about noticing when a
//! meeting ends, not about power.
#define POWER_SAVE_RECHECK_MS (30 * 1000)
//! Mic-conflict retry backoff. Starts quickly, because the common cause -- dictation holding the
//! microphone -- clears in seconds; settles slowly, because the other cause is heap pressure and
//! retrying hard under that makes it worse.
#define CAPTURE_RETRY_MIN_MS (1000)
#define CAPTURE_RETRY_MAX_MS (30 * 1000)
//! How long a continuously suppressed silence runs before the watch probes its receiver.
//!
//! While suppressing, the watch deliberately sends nothing at all -- which is exactly why
//! prv_check_receiver_liveness_locked() skips its check: on iOS a perfectly healthy receiver is
//! suspended during the quiet, precisely BECAUSE no notifications are arriving to wake it, and
//! stopping capture on it would be a bug far worse than the one the check prevents.
//!
//! But "skipped" had become "off". prv_drain_locked() -- the only caller of that check -- stops
//! its own timer once suppressing with nothing pending, so a receiver that really was gone (the
//! app force-quit, the phone rebooted) left the microphone running and the state reading
//! "Streaming" indefinitely, which is the one outcome the check exists to prevent.
//!
//! So probe rather than assume. One control notification wakes a suspended iOS app, whose
//! keepalive then re-arms liveness through the ordinary path; capture stops only if the probe
//! itself goes unanswered.
#define SILENCE_PROBE_INTERVAL_MS (60 * 1000)
#define CONSENT_TIMEOUT_MS (AUDIO_COMPANION_CONSENT_TIMEOUT_SECONDS * 1000)
#define LOW_BATTERY_RESUME_HYSTERESIS_PCT (5)
//! Alert when this many frames have been lost since the last alert (30 s of audio).
#define LOSS_ALERT_THRESHOLD_FRAMES (30 * 1000 / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS)
#define LOSS_ALERT_MIN_INTERVAL_SECONDS (6 * 60 * 60)
//! Silence suppression: how the watch decides nobody is talking.
//!
//! Two properties of the measured statistic, both easy to get wrong:
//!
//! 1. It is taken BEFORE voice_speex.c's SPEEX_AUDIO_GAIN (x3) stage, so a threshold here is
//!    three times lower than the level of the audio that ships.
//! 2. It is a MEAN over 320 samples, not a peak. A 3 ms door click inside a 20 ms frame raises
//!    the mean by roughly a seventh of its amplitude. That dilution is what the entry rule below
//!    depends on: transients are attenuated once by the frame average and again by the window.
//!
//! BUG: the entry rule used to require `enter_frames` CONSECUTIVE frames under the threshold and
//! reset the counter to zero on the first frame at or above it. On a wrist that is unsatisfiable,
//! and the feature produced zero suppressed-silence gaps in 88 days of use. The rule is now "at
//! least `quiet_permille` of the last `window_frames`".
//!
//! BUG, and the more expensive one: resume used to be gated on `exit_threshold` (64/96/150), i.e.
//! ABOVE the quiet threshold, plus up to 5 confirming frames. Simulating the shipped constants
//! over 116.5 h of this watch's own recordings (stream FVR) put that at 52% of utterances after a
//! quiet stretch clipped, p95 clip 420 ms, and 206 utterances destroyed outright. Resume and
//! quiet are separate questions and are now separate fields: onset clipping turns out to depend
//! almost entirely on `resume_threshold`, and airtime saved almost entirely on `quiet_threshold`.
//! `resume_threshold` is BELOW `quiet_threshold`, one frame is always enough, and the resuming
//! frame is encoded and sent -- so nothing of the onset is lost. Measured cost of the alternative:
//! requiring 3 or 5 confirming frames moves p90 clip from 300 to 420 to 500 ms.
//!
//! Levels differ mainly in HOW LONG THE ROOM MUST STAY QUIET before the watch stops sending:
//! 20 s / 15 s / 5 s. On the corpus that is 4.1% / 8.7% / 13.8% of airtime saved, with zero
//! utterances destroyed at any level and a p99 onset clip of at most 40 ms.
//!
//! Thresholds are in HIGH-PASSED units (prv_silence_level): a wrist-worn mic at PDM gain 90
//! carries arm movement, sleeve contact and body-conducted noise almost entirely below 40 Hz,
//! which Speex discards and which therefore cannot be calibrated from any recording. Measuring it
//! would let the threshold be set by how the wearer moves their arm.
#define SILENCE_MODE_LIGHT_QUIET_THRESHOLD (20)
#define SILENCE_MODE_LIGHT_RESUME_THRESHOLD (16)
#define SILENCE_MODE_LIGHT_WINDOW_MS (20000)
#define SILENCE_MODE_LIGHT_QUIET_PERMILLE (980)
#define SILENCE_MODE_LIGHT_REARM_MS (2000)
#define SILENCE_MODE_BALANCED_QUIET_THRESHOLD (24)
#define SILENCE_MODE_BALANCED_RESUME_THRESHOLD (20)
#define SILENCE_MODE_BALANCED_WINDOW_MS (15000)
#define SILENCE_MODE_BALANCED_QUIET_PERMILLE (980)
#define SILENCE_MODE_BALANCED_REARM_MS (1000)
#define SILENCE_MODE_AGGRESSIVE_QUIET_THRESHOLD (24)
#define SILENCE_MODE_AGGRESSIVE_RESUME_THRESHOLD (22)
#define SILENCE_MODE_AGGRESSIVE_WINDOW_MS (5000)
#define SILENCE_MODE_AGGRESSIVE_QUIET_PERMILLE (980)
#define SILENCE_MODE_AGGRESSIVE_REARM_MS (1000)
//! One-pole DC/rumble blocker applied before the mean: y[n] = x[n] - x[n-1] + (A*y[n-1]) >> 15.
//! A = 32440 puts the corner near 20 Hz at 16 kHz, which is below anything a person says and
//! above the wrist noise the threshold must not be measuring.
#define SILENCE_HP_COEFF_Q15 (32440)
//! How long a suppression run must last before it is worth renegotiating the BLE connection
//! interval for.
//!
//! BUG: this was done on the suppression edge itself. At the measured 60-200 suppression episodes
//! an hour that is 120-400 connection-parameter renegotiations an hour, which costs more radio
//! time than the payload the short episodes save. Runs shorter than this simply stay at the
//! streaming interval; the ones long enough to matter still get the relaxed one.
#define SILENCE_BLE_RELAX_MS (30 * 1000)
#define SILENCE_BLE_RELAX_FRAMES \
  (SILENCE_BLE_RELAX_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS)
//! Longest arming window any mode asks for, which sizes the ring below (one bit per frame).
#define SILENCE_MODE_MAX_WINDOW_MS                                            \
  (SILENCE_MODE_LIGHT_WINDOW_MS > SILENCE_MODE_BALANCED_WINDOW_MS             \
       ? (SILENCE_MODE_LIGHT_WINDOW_MS > SILENCE_MODE_AGGRESSIVE_WINDOW_MS    \
              ? SILENCE_MODE_LIGHT_WINDOW_MS                                  \
              : SILENCE_MODE_AGGRESSIVE_WINDOW_MS)                            \
       : (SILENCE_MODE_BALANCED_WINDOW_MS > SILENCE_MODE_AGGRESSIVE_WINDOW_MS \
              ? SILENCE_MODE_BALANCED_WINDOW_MS                               \
              : SILENCE_MODE_AGGRESSIVE_WINDOW_MS))
#define SILENCE_WINDOW_MAX_FRAMES \
  (SILENCE_MODE_MAX_WINDOW_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS)

typedef struct {
  bool data_subscribed;
  bool control_subscribed;
  bool authorized;
  uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES];
  uint8_t granted_proto_version;
} ReceiverSession;

typedef struct {
  bool pending;
  uint8_t request_token;
  uint8_t proto_version;
  uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES];
  char name[AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES + 1];
} ConsentRequest;

typedef struct {
  bool pending;
  uint8_t request_token;
} EnableRequest;

typedef struct {
  //! Level below which a frame counts as quiet, for the entry rule.
  uint32_t quiet_threshold;
  //! Level at or above which suppression ends. BELOW quiet_threshold on purpose: it is the knob
  //! that decides how much speech gets clipped, and it should be as low as the noise floor allows.
  uint32_t resume_threshold;
  //! Trailing window the entry rule looks at, in 20 ms frames.
  uint32_t window_frames;
  //! Fraction of that window (per mille) that must be quiet to suppress.
  uint32_t quiet_permille;
  //! Consecutive quiet frames required before (re-)arming, on top of the window test. Without it
  //! the rule degenerates into a per-frame transmit gate that suppresses the pauses between words
  //! ~2,700 times an hour.
  uint32_t rearm_quiet_frames;
} SilenceModeConfig;

static PBL_MUTEX_DEFINE(s_lock);
static bool s_initialized;
static AudioCompanionServiceState s_state = AudioCompanionServiceStateDisabled;
static bool s_enabled;
static ReceiverSession s_session;
static ConsentRequest s_consent;
static EnableRequest s_enable_request;
static AudioCompanionConsentHandler s_consent_handler;
static AudioCompanionEnableHandler s_enable_handler;
static TimerID s_consent_timer = TIMER_INVALID_ID;
static TimerID s_enable_timer = TIMER_INVALID_ID;

// Stream + capture state
//!
//! Capture intent vs. reality. The mic driver calls prv_mic_data_handler() with its own mutex
//! held, and that handler takes s_lock -- so on the capture path the driver mutex is always
//! acquired *before* s_lock. Calling mic_start()/mic_stop() while holding s_lock takes the same
//! two locks in the opposite order, so any task that did that (dictation from the App task,
//! runlevel/battery changes from KernelMain, the settings UI) could deadlock against KernelBG
//! sitting mid-dispatch. KernelBG then never feeds the task watchdog again and the watch resets
//! ~7 s later with RebootReasonCode_Watchdog.
//!
//! So the state machine only records *intent* (s_owns_mic / s_capture_wanted) under s_lock, and
//! prv_apply_capture() performs the driver calls with s_lock dropped.
//!
//! Since upstream v4.36 every pbl_mutex is recursive, where s_lock used to be a non-recursive
//! PebbleMutex. Re-entering s_lock on one task therefore nests silently instead of deadlocking on
//! the spot, so the lock no longer catches a violation of the "dropped before the driver call"
//! rule by itself -- the ordering above is now a convention the tests enforce
//! (prv_assert_service_lock_not_held in test_audio_companion.c runs on every driver call). Keep it
//! that way: a nested acquire here would let a re-entrant path observe half-applied state instead
//! of stopping, and the failure would surface as a watchdog reset with no obvious cause.
static bool s_owns_mic;                //!< service intends to hold the mic (guarded by s_lock)
static bool s_capture_wanted;          //!< intent handed to prv_apply_capture() (guarded by s_lock)
static bool s_mic_started;             //!< driver really started; owned by prv_apply_capture()
static PBL_MUTEX_DEFINE(s_capture_lock);    //!< serializes appliers; never held while taking s_lock
//! Desired BLE responsiveness, applied outside s_lock by prv_apply_ble_responsiveness().
static ResponseTimeState s_response_state_desired = ResponseTimeMax;
static uint16_t s_response_period_desired;
static bool s_response_dirty;
//! Guards s_drain_cb_pending alone. The drain timer fires on the NewTimers task, which must never
//! block on s_lock: that task also runs the regular-timer callback that feeds KernelBG's watchdog
//! bit while it is idle, so stalling it takes out both watchdogged tasks. This lock is only ever
//! held for a flag read-modify-write, with nothing nested inside it.
static PBL_MUTEX_DEFINE(s_drain_post_lock);
static bool s_stream_active;
static bool s_need_stream_start;
static bool s_stream_resumed;    //!< the pending STREAM_START re-announces an ongoing stream
static bool s_stream_attached;   //!< current ready session has been (re)announced the stream
static uint32_t s_stream_id;
static uint32_t s_next_sequence;
static uint64_t s_next_sample_index;
static uint64_t s_stream_start_time_ms;       //!< wall clock captured at stream birth
static uint64_t s_stream_start_monotonic_ms;  //!< uptime captured at stream birth
static TimerID s_drain_timer = TIMER_INVALID_ID;
static TimerID s_catch_up_timer = TIMER_INVALID_ID;
static bool s_catch_up_burst;
//! Notifications this drain slice may send; ramps up under backlog, collapses on backpressure.
static uint32_t s_drain_burst_cap = DRAIN_MAX_BATCHES_PER_CALL;
static bool s_drain_cb_pending;        //!< a drain callback is queued; coalesces drain posts
static bool s_capture_parked;          //!< capture stopped after offline overflow
static uint32_t s_pause_started_ms;    //!< uptime when a gap-producing pause began
static uint8_t s_pending_resume_gap_reason;
static uint32_t s_last_receiver_activity_ms;  //!< uptime of the last control message received
static uint32_t s_offline_since_ms;    //!< uptime capture went offline; 0 while a receiver is ready
static bool s_receiver_presumed_gone;  //!< liveness watchdog tripped; treat session as not ready
static TimerID s_silence_probe_timer = TIMER_INVALID_ID;
static bool s_stationary_muted;        //!< stationary AND quiet: the mic is off to save the PDM rails
static TimerID s_power_save_listen_timer = TIMER_INVALID_ID;
static TimerID s_capture_retry_timer = TIMER_INVALID_ID;
static uint32_t s_capture_retry_delay_ms;  //!< backoff for the mic-conflict retry; 0 = disarmed
static bool s_silence_probe_outstanding;  //!< a suppressed-silence probe is awaiting an answer

// Policy inputs
static bool s_mic_conflict_active;
static bool s_receiver_pause_requested;
static bool s_low_battery;
static bool s_pause_stationary_enabled;
static bool s_pause_low_power_enabled;
static AudioCompanionSilenceMode s_silence_mode = AudioCompanionSilenceModeLight;
static RunLevel s_runlevel = RunLevel_Normal;
static bool s_error;

// Diagnostics
static uint32_t s_captured_frames;
static uint32_t s_sent_frames;
static uint32_t s_send_backpressure_events;
static uint32_t s_mic_conflicts;
static uint32_t s_suppressed_silence_frames;
static uint32_t s_loss_alerts_posted;
static uint32_t s_alert_baseline_dropped;
static uint32_t s_last_alert_uptime_s;
static uint32_t s_offline_baseline_dropped;
static bool s_silence_suppressing;
static uint32_t s_silence_gap_frames;
static uint32_t s_silence_gap_first_sequence;
static uint64_t s_silence_gap_first_sample_index;
//! Number of suppression runs started since init, and the mean-abs of the most recent frame.
//! Both exist to be READ: the counter above was recorded for three months and never displayed
//! anywhere, so nobody could tell a detector that was working from one that could not fire.
static uint32_t s_silence_runs;
static uint32_t s_silence_last_level;      //!< high-passed mean-abs of the most recent frame
//! Raw (not high-passed) mean-abs of the same frame, kept ONLY so a person can read both numbers
//! off the watch. Simulation against 116.5 h of these recordings could not reconcile the observed
//! "never fires" with the thresholds by a factor of 9 (19 dB), and sub-40 Hz wrist noise -- which
//! Speex discards, so no recording can show it -- is the leading explanation. Two numbers side by
//! side in Settings settle it in one glance. It costs two instructions per sample inside a loop
//! that already loads every sample.
static uint32_t s_silence_last_raw_level;
static uint32_t s_silence_quiet_run;       //!< consecutive quiet frames, for the re-arm hangover
static int32_t s_silence_hp_x1;            //!< one-pole high-pass memory: previous input sample
static int32_t s_silence_hp_y_q8;          //!< ...and previous output, held in Q8 (see below)

//! Trailing "was this frame quiet" ring for the entry rule, one bit per 20 ms frame, plus the
//! running population count so the test is O(1) per frame rather than a rescan. 32 bytes of .bss
//! for the longest window any mode configures.
static uint8_t s_silence_window_bits[(SILENCE_WINDOW_MAX_FRAMES + 7) / 8];
static uint16_t s_silence_window_head;    //!< next bit index to overwrite
static uint16_t s_silence_window_filled;  //!< frames seen since the last re-arm, capped at window
static uint16_t s_silence_window_quiet;   //!< quiet frames currently inside the window

static EventServiceInfo s_battery_event_info;

// Reboot flight recorder (cached copy of the persisted ring; populated once at boot).
static AudioCompanionRebootTrace s_reboot_trace;
static bool s_reboot_trace_recorded;

static void prv_reevaluate_locked(void);
static void prv_drain_system_task_cb(void *data);
static void prv_post_drain_cb(void);
static void prv_start_drain_timer_locked(void);
static void prv_stop_drain_timer_locked(void);
static void prv_arm_silence_probe_locked(uint32_t delay_ms);
static void prv_stop_silence_probe_locked(void);
static void prv_arm_power_save_listen_locked(uint32_t delay_ms);
static void prv_stop_power_save_listen_locked(void);
static void prv_arm_capture_retry_locked(void);
static void prv_stop_capture_retry_locked(void);
static void prv_update_stationary_mute_locked(void);
static void prv_update_ble_responsiveness_locked(void);
static void prv_apply_pending(void);
static const SilenceModeConfig *prv_silence_mode_config(AudioCompanionSilenceMode mode);
static bool prv_room_is_quiet_locked(void);

// ---- Small helpers ----

static uint32_t prv_uptime_ms(void) { return time_get_uptime_seconds() * 1000; }

static uint64_t prv_wall_clock_ms(void) {
  time_t seconds = 0;
  uint16_t ms = 0;
  rtc_get_time_ms(&seconds, &ms);
  return (uint64_t)seconds * 1000 + ms;
}

static bool prv_session_ready_locked(void) {
  return s_session.authorized && s_session.data_subscribed && !s_receiver_presumed_gone;
}

//! Background capture only runs at RunLevel_Normal. Stationary and LowPower pause unconditionally
//! (deliberate: the pause is not user-overridable), and BareMinimum / FirmwareUpdate must stop the
//! mic too -- those runlevels are entered for panics, factory resets and firmware updates, where
//! holding the mic and the Speex encoder open is exactly the wrong thing to do.
//! Which runlevels take the microphone away.
//!
//! Three of the four are not negotiable and pause unconditionally: BareMinimum is the panic/reset
//! path, FirmwareUpdate must not contend for anything, LowPower is genuine critical battery.
//!
//! RunLevel_Stationary is the interesting one, and it is worth keeping: the Session 17 battery
//! audit found that stopping the microphone is the single largest win available to us, because
//! mic_start() holds continuous PDM capture, buffers, clocks and PMIC rails that silence
//! suppression cannot release — suppression only saves the encoder and the radio. So stationary
//! pausing stays ON, automatically, with no setting.
//!
//! What it must not do is trust wrist motion ALONE. The stationary service engages after thirty
//! minutes without meaningful movement off the charger, and for a background audio recorder that
//! describes a meeting, a lecture, a film — sitting still is not the same as nothing being said,
//! and the watch was switching its microphone off through exactly the recordings this product
//! exists to make. Worse, nothing brought it back: stationary only ends on a shake or a button.
//!
//! So the gate asks for the OTHER half of the evidence, which the watch already computes. Pause
//! only when the wrist is still AND the room is quiet — `s_silence_suppressing`, the voice
//! detector that is already deciding, frame by frame, whether anyone is speaking. Still and quiet
//! is a nightstand, and the mic should stop. Still and talking is a meeting, and it must not.
//!
//! `prv_arm_power_save_listen_locked()` supplies the way back: while muted for stationary the
//! watch briefly reopens the microphone on a slow cadence, so a conversation that starts while
//! the wrist stays still is picked up within a couple of minutes instead of never.
static bool prv_power_save_active_locked(void) {
  switch (s_runlevel) {
    case RunLevel_BareMinimum:
    case RunLevel_FirmwareUpdate:
    case RunLevel_LowPower:
      return true;
    case RunLevel_Stationary:
      return s_stationary_muted;
    default:
      return false;
  }
}

//! Decides, for the stationary runlevel only, whether we are currently muted.
//!
//! Split from the gate above so the listen probe can flip it without the gate needing to know
//! anything about timers. Called on every runlevel change and at the end of each listen window.
//!
//! Note the coupling this creates, which is deliberate but worth knowing: with Skip Silence set
//! to Off the detector never suppresses, so the stationary mute never engages either and the
//! microphone runs through every still moment. Off costs battery for exactly the reason it is
//! safe — the watch has no evidence the room is empty, so it does not act as if it were.
static void prv_update_stationary_mute_locked(void) {
  if (s_runlevel != RunLevel_Stationary) {
    if (s_stationary_muted) {
      s_stationary_muted = false;
    }
    prv_stop_power_save_listen_locked();
    return;
  }
  // Quiet as well as still: mute and start the slow listen cadence. Speech in the room means this
  // is a false positive for "nothing worth recording", so keep capturing and check again later.
  s_stationary_muted = prv_room_is_quiet_locked();
  prv_arm_power_save_listen_locked(s_stationary_muted ? POWER_SAVE_LISTEN_INTERVAL_MS
                                                      : POWER_SAVE_RECHECK_MS);
}

static const SilenceModeConfig *prv_silence_mode_config(AudioCompanionSilenceMode mode) {
  static const SilenceModeConfig s_configs[] = {
    [AudioCompanionSilenceModeLight] = {
      .quiet_threshold = SILENCE_MODE_LIGHT_QUIET_THRESHOLD,
      .resume_threshold = SILENCE_MODE_LIGHT_RESUME_THRESHOLD,
      .window_frames = SILENCE_MODE_LIGHT_WINDOW_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
      .quiet_permille = SILENCE_MODE_LIGHT_QUIET_PERMILLE,
      .rearm_quiet_frames =
          SILENCE_MODE_LIGHT_REARM_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
    },
    [AudioCompanionSilenceModeBalanced] = {
      .quiet_threshold = SILENCE_MODE_BALANCED_QUIET_THRESHOLD,
      .resume_threshold = SILENCE_MODE_BALANCED_RESUME_THRESHOLD,
      .window_frames = SILENCE_MODE_BALANCED_WINDOW_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
      .quiet_permille = SILENCE_MODE_BALANCED_QUIET_PERMILLE,
      .rearm_quiet_frames =
          SILENCE_MODE_BALANCED_REARM_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
    },
    [AudioCompanionSilenceModeAggressive] = {
      .quiet_threshold = SILENCE_MODE_AGGRESSIVE_QUIET_THRESHOLD,
      .resume_threshold = SILENCE_MODE_AGGRESSIVE_RESUME_THRESHOLD,
      .window_frames =
          SILENCE_MODE_AGGRESSIVE_WINDOW_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
      .quiet_permille = SILENCE_MODE_AGGRESSIVE_QUIET_PERMILLE,
      .rearm_quiet_frames =
          SILENCE_MODE_AGGRESSIVE_REARM_MS / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
    },
  };
  _Static_assert(SILENCE_MODE_LIGHT_RESUME_THRESHOLD <= SILENCE_MODE_LIGHT_QUIET_THRESHOLD &&
                 SILENCE_MODE_BALANCED_RESUME_THRESHOLD <= SILENCE_MODE_BALANCED_QUIET_THRESHOLD &&
                 SILENCE_MODE_AGGRESSIVE_RESUME_THRESHOLD <=
                     SILENCE_MODE_AGGRESSIVE_QUIET_THRESHOLD,
                 "resume_threshold above quiet_threshold clips speech for no saving");
  if (mode <= AudioCompanionSilenceModeOff || mode >= AudioCompanionSilenceModeCount) {
    return NULL;
  }
  return &s_configs[mode];
}

//! Empty the trailing window. Called whenever suppression ends or capture restarts, which is what
//! makes re-entry cost a full fresh window of real audio.
//!
//! That is the flap guard. Exit is instant by design (no clipped speech), so without it a single
//! loud frame inside a quiet room would leave the window still 99% quiet, re-enter on the very
//! next frame, and produce an endless stream of one-frame gap records — each of which also
//! renegotiates the BLE connection interval (prv_update_ble_responsiveness_locked). Requiring the
//! window to refill bounds a suppression run, and therefore that renegotiation, to at most one
//! per arming window.
static void prv_silence_window_reset_locked(void) {
  s_silence_quiet_run = 0;
  if (s_silence_window_filled == 0) {
    return;  // already empty; this runs on the 50 Hz mic path with Skip Silence off
  }
  memset(s_silence_window_bits, 0, sizeof(s_silence_window_bits));
  s_silence_window_head = 0;
  s_silence_window_filled = 0;
  s_silence_window_quiet = 0;
}

//! The window plus the high-pass memory. Only for the points where the audio itself stops (capture
//! stopping, a stream beginning): clearing the filter mid-stream would inject a transient of its
//! own into the very measurement it feeds.
static void prv_silence_detector_reset_locked(void) {
  prv_silence_window_reset_locked();
  s_silence_hp_x1 = 0;
  s_silence_hp_y_q8 = 0;
}

//! Record one frame's verdict. O(1): evict one bit, store one bit, adjust one counter.
static void prv_silence_window_push_locked(bool quiet, uint32_t window_frames) {
  if (s_silence_window_filled > window_frames || s_silence_window_head >= window_frames) {
    // The window shrank under us -- a mode change that did not reset. The bits are still there but
    // they no longer mean what the new configuration would read them to mean, so start over rather
    // than arm on a count collected under a different rule.
    prv_silence_window_reset_locked();
  }
  const uint16_t head = s_silence_window_head;
  uint8_t *const byte = &s_silence_window_bits[head >> 3];
  const uint8_t mask = (uint8_t)(1u << (head & 7));
  if (s_silence_window_filled >= window_frames) {
    if (*byte & mask) {
      s_silence_window_quiet--;
    }
  } else {
    s_silence_window_filled++;
  }
  if (quiet) {
    *byte = (uint8_t)(*byte | mask);
    s_silence_window_quiet++;
  } else {
    *byte = (uint8_t)(*byte & (uint8_t)~mask);
  }
  s_silence_window_head = (uint16_t)(((head + 1u) >= window_frames) ? 0u : (head + 1u));
}

//! Integer-only fraction test: quiet/window >= permille/1000, with no division and no overflow
//! (window <= 250 frames, permille <= 1000, so the products stay well inside uint32_t).
static bool prv_silence_window_armed_locked(const SilenceModeConfig *config) {
  if (s_silence_window_filled < config->window_frames) {
    return false;
  }
  return ((uint32_t)s_silence_window_quiet * 1000u) >=
         (config->window_frames * config->quiet_permille);
}

//! Quiet fraction of the window so far, per mille, for the diagnostics readout.
static uint16_t prv_silence_window_permille_locked(void) {
  if (s_silence_window_filled == 0) {
    return 0;
  }
  return (uint16_t)(((uint32_t)s_silence_window_quiet * 1000u) / s_silence_window_filled);
}

//! "Is this room quiet", for the stationary power-save mute only.
//!
//! Narrower than the suppression question, deliberately. Suppression must not stop transmitting
//! until it is very sure, so its window runs 5-20 s; the mute only has to decide whether to sleep
//! the microphone for one listen interval, and being wrong costs at most that interval of a
//! conversation which the next listen window then picks up. So it judges the same threshold at the
//! same 98% bar over however much the listen window actually heard -- which keeps the microphone
//! on for 6 s of every 2 minutes instead of 22.
static bool prv_room_is_quiet_locked(void) {
  const SilenceModeConfig *config = prv_silence_mode_config(s_silence_mode);
  if (!config) {
    return false;  // Skip Silence off: no evidence, so never sleep the microphone on a guess
  }
  if (s_silence_suppressing) {
    return true;
  }
  if (s_silence_window_filled < POWER_SAVE_LISTEN_MIN_FRAMES) {
    return false;
  }
  return ((uint32_t)s_silence_window_quiet * 1000u) >=
         ((uint32_t)s_silence_window_filled * config->quiet_permille);
}

//! End the current suppression run WITHOUT touching the trailing window.
//!
//! The window deliberately survives a resume. A cough or a door then costs only the frames it
//! occupies plus `rearm_quiet_frames`, rather than a whole fresh 15-20 s window -- which on this
//! corpus is the difference between 1.6 and 4.2 hours of long suppression episodes. The full
//! detector reset belongs where capture actually stops (prv_stop_capture_locked) and where a
//! stream begins, because that is where the audio it describes goes away.
static void prv_end_silence_run_locked(void) {
  const bool was_suppressing = s_silence_suppressing;
  s_silence_suppressing = false;
  s_silence_gap_frames = 0;
  s_silence_gap_first_sequence = 0;
  s_silence_gap_first_sample_index = 0;
  if (was_suppressing) {
    prv_stop_silence_probe_locked();
    prv_update_ble_responsiveness_locked();
  }
}

static void prv_reset_silence_suppression_locked(void) {
  prv_end_silence_run_locked();
  prv_silence_detector_reset_locked();
}

static void prv_record_silence_gap_locked(void) {
  if (!s_silence_suppressing || s_silence_gap_frames == 0) {
    prv_end_silence_run_locked();
    return;
  }
  audio_companion_spool_record_gap(s_silence_gap_first_sequence, s_silence_gap_frames,
                                   s_silence_gap_first_sample_index,
                                   AudioCompanionGapReasonSilenceSuppressed);
  // Suppressed silence intentionally has no receiver traffic. Re-arm liveness when traffic
  // resumes so the receiver gets a fresh window to persist and checkpoint the next notification.
  s_last_receiver_activity_ms = prv_uptime_ms();
  prv_end_silence_run_locked();
}

//! Mean absolute level of one frame, after a one-pole high-pass.
//!
//! The high-pass is not cosmetic. Without it the detector measures whatever the wrist is doing --
//! arm movement, sleeve contact, body-conducted thump, all of it below 40 Hz at PDM digital gain
//! 90 -- and none of that survives Speex, so it is invisible in every recording we have and no
//! threshold derived from one is meaningful. Simulation over 116.5 h could not reconcile "fires
//! zero times at threshold 40" with the decoded audio by a factor of 9 (19 dB), and this is the
//! leading explanation. y[n] = x[n] - x[n-1] + (A * y[n-1]) >> 15, ~20 Hz corner at 16 kHz.
//!
//! The raw mean is accumulated in the same pass (two extra instructions per sample, no second
//! load) purely so the watch can show both numbers and settle that question on a real wrist.
static uint32_t prv_silence_level(const int16_t *samples, size_t sample_count,
                                  uint32_t *raw_mean_out) {
  int32_t x1 = s_silence_hp_x1;
  int32_t y_q8 = s_silence_hp_y_q8;
  uint32_t sum_q4 = 0;
  uint32_t raw_sum = 0;
  for (size_t i = 0; i < sample_count; i++) {
    const int32_t x = samples[i];
    // Whole-integer state would LATCH here, which is the same class of bug as the one this whole
    // change exists to fix. With y held as a plain int and a pole at 0.99, `(A * y) >> 15` is an
    // arithmetic shift: it floors, so for y = -99 it returns -99 and the filter never decays. A
    // single negative-going DC step -- an arm dropping to a desk -- would pin the reported level
    // at ~99, five times the quiet threshold, for the rest of the session. Q8 state plus a
    // truncating divide decays to zero from either sign.
    y_q8 = (int32_t)(((x - x1) << 8) +
                     (int32_t)(((int64_t)SILENCE_HP_COEFF_Q15 * y_q8) / 32768));
    x1 = x;
    const int32_t magnitude = (y_q8 < 0) ? -y_q8 : y_q8;
    sum_q4 += (uint32_t)magnitude >> 4;  // Q4: 320 x (2^24 >> 4) still leaves 12x uint32 headroom
    raw_sum += (x < 0) ? (uint32_t)-x : (uint32_t)x;
  }
  s_silence_hp_x1 = x1;
  s_silence_hp_y_q8 = y_q8;
  if (raw_mean_out) {
    *raw_mean_out = sample_count ? (raw_sum / sample_count) : 0;
  }
  return sample_count ? ((sum_q4 / sample_count) >> 4) : 0;
}

static bool prv_maybe_suppress_silence_locked(const int16_t *samples, size_t sample_count,
                                              bool *out_schedule_drain) {
  const SilenceModeConfig *config = prv_silence_mode_config(s_silence_mode);
  if (!config) {
    if (s_silence_suppressing && out_schedule_drain) {
      *out_schedule_drain = true;
    }
    prv_record_silence_gap_locked();
    return false;
  }

  const uint32_t level = prv_silence_level(samples, sample_count, &s_silence_last_raw_level);
  s_silence_last_level = level;
  const bool quiet = (level < config->quiet_threshold);
  s_silence_quiet_run = quiet ? (s_silence_quiet_run + 1) : 0;

  if (s_silence_suppressing) {
    if (level >= config->resume_threshold) {
      // One frame is always enough, and THIS frame is encoded and sent -- so the only speech at
      // risk is whatever preceded the first frame above resume_threshold. Requiring confirmation
      // frames instead measured at 300-500 ms of p90 onset clipping.
      prv_record_silence_gap_locked();
      // The window keeps running across the resume; feed it this frame and carry on.
      prv_silence_window_push_locked(quiet, config->window_frames);
      if (out_schedule_drain) {
        *out_schedule_drain = true;
      }
      return false;
    }
  } else {
    // The window only ever describes audio we actually sent, so it is not fed while suppressing.
    prv_silence_window_push_locked(quiet, config->window_frames);
    // Never arm on a frame we would then suppress: that would clip 20 ms for nothing. Redundant
    // while every mode's rearm_quiet_frames is at least 1 (a loud frame zeroes the run), and kept
    // because a mode that ever set it to 0 would otherwise silently start clipping.
    if (!quiet || s_silence_quiet_run < config->rearm_quiet_frames ||
        !prv_silence_window_armed_locked(config)) {
      return false;
    }
    s_silence_suppressing = true;
    s_silence_runs++;
    s_silence_gap_frames = 0;
    s_silence_gap_first_sequence = s_next_sequence;
    s_silence_gap_first_sample_index = s_next_sample_index;
    PBL_LOG_DBG("Audio companion: suppressing silence (mode %u, level %" PRIu32 "/%" PRIu32
                " hp/raw, quiet < %" PRIu32 ")",
                (unsigned)s_silence_mode, level, s_silence_last_raw_level,
                config->quiet_threshold);
    if (out_schedule_drain) {
      // The mic path cannot touch the transport itself (it holds the mic driver mutex). Posting a
      // drain hands the bookkeeping -- standing the drain timer down, arming the silence probe --
      // to the system task. The connection interval deliberately does NOT change here; see
      // SILENCE_BLE_RELAX_FRAMES below.
      *out_schedule_drain = true;
    }
  }

  s_next_sequence++;
  s_next_sample_index += sample_count;
  s_captured_frames++;
  s_suppressed_silence_frames++;
  s_silence_gap_frames++;
  if (s_silence_gap_frames == SILENCE_BLE_RELAX_FRAMES) {
    // Long enough to be worth a connection-parameter renegotiation. Short runs never pay for one.
    prv_update_ble_responsiveness_locked();
    if (out_schedule_drain) {
      *out_schedule_drain = true;
    }
  }
  return true;
}

//! Mark the receiver as alive: called whenever a control message or a fresh subscription
//! arrives. Clears a tripped liveness watchdog.
static void prv_note_receiver_activity_locked(void) {
  s_last_receiver_activity_ms = prv_uptime_ms();
  s_offline_since_ms = 0;
  // BUG: this used to assign TIMER_INVALID_ID to the power-save listen and capture-retry handles.
  // That does not stop a timer, it forgets one. The timer stayed scheduled and its slot in
  // task_timer.c's fixed pool was never returned, while the next arm allocated a fresh slot with
  // new_timer_create(); pool exhaustion there is a PBL_ASSERTN, so a watch that entered
  // RunLevel_Stationary often enough would eventually panic on a timer allocation. The receiver
  // talks to us every 0.5-2 s while streaming, so this ran constantly. Stop the retry properly.
  prv_stop_capture_retry_locked();
  // The stationary mute is deliberately NOT cleared here. It is a statement about the room (still
  // AND quiet), not about the phone, and clearing it without re-arming the listen cadence left
  // the state machine stranded: unmuted, no timer, and nothing to re-ask until the runlevel
  // happened to change again.
  s_receiver_presumed_gone = false;
  if (s_silence_probe_outstanding) {
    // The probe did its job: the receiver was asleep, not gone. Go back to the long cadence.
    s_silence_probe_outstanding = false;
    if (s_silence_suppressing) {
      prv_arm_silence_probe_locked(SILENCE_PROBE_INTERVAL_MS);
    }
  }
}

static void prv_notify_control_locked(const uint8_t *data, size_t length) {
  if (!s_session.control_subscribed) {
    return;
  }
  bt_driver_audio_companion_notify_control(data, length);
}

static void prv_send_state_changed_locked(void) {
  uint8_t buf[sizeof(AudioCompanionStateChangedMsg)];
  const size_t len =
      audio_companion_protocol_build_state_changed(buf, sizeof(buf), (uint8_t)s_state);
  prv_notify_control_locked(buf, len);
}

static void prv_cancel_catch_up_burst_locked(void) {
  if (s_catch_up_burst) {
    s_catch_up_burst = false;
    if (s_catch_up_timer != TIMER_INVALID_ID) {
      new_timer_stop(s_catch_up_timer);
    }
  }
}

//! Record the responsiveness the current state wants. The driver call itself takes bt_lock, which
//! the BT stack can hold for seconds, and s_lock must not be held across it -- see
//! prv_apply_ble_responsiveness().
static void prv_update_ble_responsiveness_locked(void) {
  ResponseTimeState state = ResponseTimeMax;
  uint16_t period = 0;
  if (s_state == AudioCompanionServiceStateStreaming && s_catch_up_burst) {
    state = ResponseTimeMin;
    period = MIN_LATENCY_MODE_TIMEOUT_AUDIO_SECS;
  } else if (s_state == AudioCompanionServiceStateStreaming &&
             !(s_silence_suppressing && s_silence_gap_frames >= SILENCE_BLE_RELAX_FRAMES)) {
    state = ResponseTimeMiddle;
    period = MAX_PERIOD_RUN_FOREVER;
  }
  if (state != s_response_state_desired || period != s_response_period_desired) {
    s_response_state_desired = state;
    s_response_period_desired = period;
    s_response_dirty = true;
  }
}

//! Push the recorded responsiveness to the BT driver with s_lock released.
//!
//! bt_driver_audio_companion_set_response_time() takes bt_lock, a global the BT stack holds across
//! HCI round trips and bonding flash I/O. Holding s_lock across that is what makes s_lock a
//! multi-second lock, and s_lock is on the watchdog-critical path: the drain timer callback runs
//! on the NewTimers task, which also drives the regular-timer seconds callbacks that feed
//! KernelBG's watchdog bit while it is idle. Stall NewTimers and both watchdogged tasks go
//! unfed -- which is the reset this feature has been causing.
static void prv_apply_ble_responsiveness(void) {
  if (!s_initialized) {
    return;
  }
  for (;;) {
    pbl_mutex_lock(&s_lock, PBL_FOREVER);
    const bool dirty = s_response_dirty;
    const ResponseTimeState state = s_response_state_desired;
    const uint16_t period = s_response_period_desired;
    s_response_dirty = false;
    pbl_mutex_unlock(&s_lock);

    if (!dirty) {
      return;
    }
    bt_driver_audio_companion_set_response_time(state, period);
  }
}

static void prv_end_catch_up_burst_system_task_cb(void *data) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_catch_up_burst = false;
  if (s_catch_up_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_catch_up_timer);
  }
  prv_update_ble_responsiveness_locked();
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

static void prv_catch_up_timer_cb(void *data) {
  system_task_add_callback(prv_end_catch_up_burst_system_task_cb, NULL);
}

static void prv_begin_catch_up_burst_locked(void) {
  if (s_catch_up_burst) {
    return;
  }
  s_catch_up_burst = true;
  prv_update_ble_responsiveness_locked();
  if (s_catch_up_timer == TIMER_INVALID_ID) {
    s_catch_up_timer = new_timer_create();
  }
  new_timer_start(s_catch_up_timer, MIN_LATENCY_MODE_TIMEOUT_AUDIO_SECS * 1000,
                  prv_catch_up_timer_cb, NULL, 0);
}

static void prv_set_state_locked(AudioCompanionServiceState state) {
  if (s_state == state) {
    return;
  }
  if (state != AudioCompanionServiceStateStreaming) {
    prv_cancel_catch_up_burst_locked();
  }
  PBL_LOG_DBG("Audio companion state %u -> %u", (unsigned)s_state, (unsigned)state);
  s_state = state;
  prv_update_ble_responsiveness_locked();
  prv_send_state_changed_locked();
}

static void prv_send_auth_result_locked(uint8_t token, uint8_t status, uint8_t granted) {
  uint8_t buf[sizeof(AudioCompanionAuthResultMsg)];
  const size_t len =
      audio_companion_protocol_build_auth_result(buf, sizeof(buf), token, status, granted);
  prv_notify_control_locked(buf, len);
}

static void prv_send_ack_locked(uint8_t token, uint8_t status) {
  uint8_t buf[sizeof(AudioCompanionAckMsg)];
  const size_t len = audio_companion_protocol_build_ack(buf, sizeof(buf), token, status);
  prv_notify_control_locked(buf, len);
}

static void prv_send_error_locked(uint8_t error_code, uint32_t detail) {
  uint8_t buf[sizeof(AudioCompanionErrorMsg)];
  const size_t len =
      audio_companion_protocol_build_error(buf, sizeof(buf), error_code, detail);
  prv_notify_control_locked(buf, len);
}

static void prv_send_revoked_locked(uint8_t reason) {
  uint8_t buf[sizeof(AudioCompanionRevokedMsg)];
  const size_t len = audio_companion_protocol_build_revoked(buf, sizeof(buf), reason);
  prv_notify_control_locked(buf, len);
}

// ---- Loss alert ----

static void prv_post_loss_alert(void) {
#ifndef UNITTEST
  AttributeList attr_list = {0};
  attribute_list_add_cstring(&attr_list, AttributeIdTitle, "Audio Companion");
  attribute_list_add_cstring(&attr_list, AttributeIdBody,
                             "Some audio could not be delivered to your audio app and was "
                             "skipped.");
  TimelineItem *item = timeline_item_create_with_attributes(
      rtc_get_time(), 0, TimelineItemTypeNotification, LayoutIdNotification, &attr_list, NULL);
  attribute_list_destroy_list(&attr_list);
  if (item) {
    item->header.from_watch = true;
    notifications_add_notification(item);
    timeline_item_destroy(item);
  }
#endif
  s_loss_alerts_posted++;
}

//! Tell the user why background audio turned itself off. Silence here would look like the setting
//! forgot itself, which is worse than the crash run it is reporting.
static void prv_post_stand_down_alert(void) {
#ifndef UNITTEST
  AttributeList attr_list = {0};
  attribute_list_add_cstring(&attr_list, AttributeIdTitle, "Audio Companion");
  attribute_list_add_cstring(&attr_list, AttributeIdBody,
                             "Background audio turned itself off after your watch restarted "
                             "several times. Settings shows what happened.");
  TimelineItem *item = timeline_item_create_with_attributes(
      rtc_get_time(), 0, TimelineItemTypeNotification, LayoutIdNotification, &attr_list, NULL);
  attribute_list_destroy_list(&attr_list);
  if (item) {
    item->header.from_watch = true;
    notifications_add_notification(item);
    timeline_item_destroy(item);
  }
#endif
}

static void prv_loss_alert_system_task_cb(void *data) {
  // Building a timeline notification allocates and touches the notification store; keep it off the
  // mic capture path (which runs under both s_lock and the mic driver's mutex on KernelBG).
  prv_post_loss_alert();
}

//! Decides (under s_lock) whether a "some audio was skipped" alert is due. Returns true if the
//! caller should post it from a plain system-task callback; the heavy notification work must not
//! run inline on the locked mic path.
static bool prv_maybe_alert_loss_locked(void) {
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  if (stats.dropped_overflow_frames < s_alert_baseline_dropped + LOSS_ALERT_THRESHOLD_FRAMES) {
    return false;
  }
  const uint32_t now_s = time_get_uptime_seconds();
  s_alert_baseline_dropped = stats.dropped_overflow_frames;
  if (s_last_alert_uptime_s != 0 &&
      (now_s - s_last_alert_uptime_s) < LOSS_ALERT_MIN_INTERVAL_SECONDS) {
    return false;
  }
  s_last_alert_uptime_s = now_s;
  PBL_LOG_WRN("Audio companion lost >%u frames; alerting user",
              (unsigned)LOSS_ALERT_THRESHOLD_FRAMES);
  return true;
}

// ---- Capture ----

static void prv_mic_data_handler(int16_t *samples, size_t sample_count, void *context);

//! Record that capture should be running. The mic driver is only touched later, by
//! prv_apply_capture(), which the caller must invoke once it has dropped s_lock.
static void prv_start_capture_locked(void) {
  s_capture_wanted = true;
  s_owns_mic = true;
  s_capture_parked = false;
  // Whatever the mic-conflict backoff had reached, this is a fresh start: reset it so the next
  // failure retries promptly rather than inheriting a 30 s delay from an old episode.
  prv_stop_capture_retry_locked();
}

//! Record that capture should stop. Like prv_start_capture_locked(), the driver call happens in
//! prv_apply_capture().
static void prv_stop_capture_locked(void) {
  s_capture_wanted = false;
  s_owns_mic = false;
  // The trailing window and the high-pass memory describe audio that is about to stop arriving.
  // Carrying them across a pause would let a verdict be formed from two different rooms.
  prv_silence_detector_reset_locked();
}

//! Reconcile the mic driver with the intent recorded under s_lock.
//!
//! MUST be called with s_lock released: this is the function that actually takes the driver's
//! mutex, and taking it under s_lock is the lock-order inversion described above. s_capture_lock
//! serializes concurrent appliers and is itself never acquired while s_lock is held, so the
//! global order stays acyclic: s_capture_lock -> mic mutex -> s_lock.
static void prv_apply_capture(void) {
  if (!s_initialized) {
    return;
  }
  pbl_mutex_lock(&s_capture_lock, PBL_FOREVER);
  for (;;) {
    pbl_mutex_lock(&s_lock, PBL_FOREVER);
    const bool want = s_capture_wanted;
    pbl_mutex_unlock(&s_lock);

    if (want == s_mic_started) {
      break;
    }

    if (!want) {
      if (mic_is_running(MIC)) {
        mic_stop(MIC);
      }
      if (voice_speex_is_initialized()) {
        voice_speex_deinit();
      }
      s_mic_started = false;
      PBL_LOG_INFO("Audio companion: capture stopped");
      continue;  // intent may have flipped again while the driver call ran
    }

    bool started = false;
    if (voice_speex_init()) {
      int16_t *frame_buffer = voice_speex_get_frame_buffer();
      const size_t frame_samples = voice_speex_get_frame_size();
      if (frame_buffer && frame_samples > 0) {
        started = mic_start(MIC, prv_mic_data_handler, NULL, frame_buffer, frame_samples);
      }
      if (!started) {
        voice_speex_deinit();
      }
    } else {
      PBL_LOG_ERR("Audio companion: speex init failed");
    }
    s_mic_started = started;

    if (started) {
      PBL_LOG_INFO("Audio companion: capture started");
      continue;
    }

    // Mic already owned by someone else (e.g. dictation) or out of memory. Park on the conflict
    // state directly rather than re-running the state machine: prv_reevaluate_locked() would set
    // the intent straight back to "capture" and spin this loop on a start that cannot succeed.
    //
    // This used to claim "the next event to reach prv_reevaluate_locked() retries". In steady
    // state there is no such event. Re-evaluation happens on a subscription change, a disconnect,
    // AUTH/consent/enable, a mic-conflict hook, a runlevel change, a battery event that CROSSES a
    // threshold, or a checkpoint whose pause flag CHANGED -- and the phone's ordinary traffic
    // hits none of them: RECEIVER_HEALTH just ACKs, and an unchanged checkpoint returns early.
    // So a transient failure -- dictation holding the mic for a few seconds, or
    // voice_speex_init()'s kernel_malloc() losing a race under heap pressure -- became a
    // permanent PausedConflict. Note it does not even set s_mic_conflict_active, so it is a pure
    // latch with no tracked input that could clear it.
    //
    // So arm a retry. Backed off so a genuinely-held mic is not hammered, and cancelled the
    // moment capture starts.
    PBL_LOG_WRN("Audio companion: mic unavailable; treating as conflict");
    pbl_mutex_lock(&s_lock, PBL_FOREVER);
    s_capture_wanted = false;
    s_owns_mic = false;
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStatePausedConflict);
    prv_arm_capture_retry_locked();
    pbl_mutex_unlock(&s_lock);
  }
  pbl_mutex_unlock(&s_capture_lock);
}

//! Flush everything the state machine deferred because it could not be done under s_lock.
//! MUST be called with s_lock released, and never from prv_mic_data_handler(): that runs with the
//! mic driver's mutex held, and prv_apply_capture() waits on that same mutex from other tasks.
static void prv_apply_pending(void) {
  prv_apply_capture();
  prv_apply_ble_responsiveness();
}

//! Begin a pause that will become a STREAM_GAP when capture resumes.
static void prv_begin_gap_pause_locked(uint8_t gap_reason) {
  if (s_pending_resume_gap_reason == 0) {
    s_pause_started_ms = prv_uptime_ms();
    s_pending_resume_gap_reason = gap_reason;
  }
}

//! Record the gap for an elapsed pause; consumes sequence numbers so gap
//! ranges and subsequent data stay self-consistent.
static void prv_finish_gap_pause_locked(void) {
  if (s_pending_resume_gap_reason == 0 || !s_stream_active) {
    s_pending_resume_gap_reason = 0;
    return;
  }
  const uint32_t elapsed_ms = prv_uptime_ms() - s_pause_started_ms;
  uint32_t missing = elapsed_ms / AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS;
  if (missing == 0) {
    missing = 1;
  }
  audio_companion_spool_record_gap(s_next_sequence, missing, s_next_sample_index,
                                   s_pending_resume_gap_reason);
  s_next_sequence += missing;
  s_next_sample_index += (uint64_t)missing * AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES;
  s_pending_resume_gap_reason = 0;
}

static void prv_park_capture_system_task_cb(void *data) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_owns_mic && !prv_session_ready_locked()) {
    PBL_LOG_DBG("Audio companion: offline too long; parking capture");
    // Close any suppression run first, exactly as every other pause path does. Leaving one open
    // would let the TransportReset pause below consume the sequence range the silence gap still
    // claims, and the receiver would be told two different stories about the same frames.
    prv_record_silence_gap_locked();
    prv_stop_capture_locked();
    s_capture_parked = true;
    prv_begin_gap_pause_locked(AudioCompanionGapReasonTransportReset);
  }
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

//! Offline bookkeeping for one captured frame; true when capture should park.
//!
//! Shared by the encoded and the suppressed paths. It used to run only on the encoded path, which
//! was harmless while the detector could never fire and is not any more: suppression stops the
//! encoder and the radio but NOT the PDM rails, buffers and clocks, which the Session 17 battery
//! audit found to be the dominant cost. A quiet room plus a receiver that is never coming back
//! would otherwise hold the microphone open indefinitely -- precisely the outcome this park was
//! written to prevent.
static bool prv_note_offline_frame_locked(void) {
  if (prv_session_ready_locked()) {
    return false;
  }
  // Offline: keep the most RECENT audio, and only give up after a long absence.
  //
  // This used to park the microphone on the very first overflowed frame, which threw away the
  // whole point of a drop-oldest ring. The spool recycles its oldest chunk quite happily, so once
  // it is full it holds a rolling window of the last ~4-67 seconds (the range is wide because the
  // ring only grows past CONFIG_AUDIO_COMPANION_SPOOL_MIN_BYTES while kernel heap headroom
  // allows). Parking at the first drop meant an outage a second longer than that window cost
  // EVERYTHING after it, not just the overflow -- and nothing un-parks capture except a receiver
  // reattaching, so a phone that was suspended for two minutes came back to a watch that had
  // stopped listening. Cycling instead means the user keeps the most recent window, which is what
  // a person expects from a background recorder, and the overflow is already reported honestly as
  // a SpoolOverflow gap.
  //
  // The park still exists, because a microphone running for a receiver that is never coming back
  // is a real battery cost. It is on a TIMER rather than on the first dropped frame:
  // OFFLINE_CAPTURE_PARK_MS of continuous absence, which is far longer than any suspension,
  // jettison or Bluetooth relaunch and short enough to matter overnight.
  if (s_offline_since_ms == 0) {
    s_offline_since_ms = prv_uptime_ms();
  }
  return (prv_uptime_ms() - s_offline_since_ms) >= OFFLINE_CAPTURE_PARK_MS;
}

static void prv_mic_data_handler(int16_t *samples, size_t sample_count, void *context) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (!s_owns_mic || !s_stream_active) {
    pbl_mutex_unlock(&s_lock);
    return;
  }
  const size_t expected_samples = (size_t)voice_speex_get_frame_size();
  if (sample_count != expected_samples) {
    pbl_mutex_unlock(&s_lock);
    return;
  }

  bool schedule_drain = false;
  if (prv_maybe_suppress_silence_locked(samples, sample_count, &schedule_drain)) {
    const bool park_suppressed = prv_note_offline_frame_locked();
    pbl_mutex_unlock(&s_lock);
    if (schedule_drain) {
      prv_post_drain_cb();
    }
    if (park_suppressed) {
      system_task_add_callback(prv_park_capture_system_task_cb, NULL);
    }
    return;
  }

  uint8_t encoded[AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES];
  const int encoded_bytes = voice_speex_encode_frame(samples, encoded, sizeof(encoded));
  if (encoded_bytes <= 0) {
    pbl_mutex_unlock(&s_lock);
    return;
  }

  const uint32_t sequence = s_next_sequence++;
  const uint64_t sample_index = s_next_sample_index;
  s_next_sample_index += expected_samples;
  s_captured_frames++;
  audio_companion_spool_push(sequence, sample_index, encoded, (uint16_t)encoded_bytes);

  if (prv_session_ready_locked()) {
    // Keep any drain already requested by silence suppression: exiting suppression must flush the
    // recorded gap (and the resuming frame) promptly. The drain timer is stopped while suppressing,
    // so overwriting this would strand the gap until enough frames re-accumulate.
    schedule_drain = schedule_drain ||
        (audio_companion_spool_frames_pending_send() >= DRAIN_PUSH_THRESHOLD_FRAMES);
  }
  const bool schedule_park = prv_note_offline_frame_locked();
  if (schedule_drain && prv_session_ready_locked()) {
    prv_start_drain_timer_locked();
  }
  const bool post_loss_alert = prv_maybe_alert_loss_locked();
  pbl_mutex_unlock(&s_lock);

  if (schedule_drain) {
    prv_post_drain_cb();
  }
  if (schedule_park) {
    system_task_add_callback(prv_park_capture_system_task_cb, NULL);
  }
  if (post_loss_alert) {
    system_task_add_callback(prv_loss_alert_system_task_cb, NULL);
  }
}

// ---- Stream / drain ----

static void prv_send_stream_start_locked(void) {
  const AudioCompanionStreamStartMsg params = {
    .protocol_version = s_session.granted_proto_version,
    .stream_id = s_stream_id,
    .codec_id = AudioCompanionCodecSpeexWideband,
    .channels = 1,
    .frame_samples = AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES,
    .sample_rate_hz = AUDIO_COMPANION_DEFAULT_SAMPLE_RATE_HZ,
    .bit_rate_bps = AUDIO_COMPANION_DEFAULT_BIT_RATE_BPS,
    .frame_duration_ms = AUDIO_COMPANION_DEFAULT_FRAME_DURATION_MS,
    .start_time_ms = s_stream_start_time_ms,
    .start_monotonic_ms = s_stream_start_monotonic_ms,
    .flags = s_stream_resumed ? AUDIO_COMPANION_STREAM_START_FLAG_RESUME : 0,
  };
  uint8_t buf[sizeof(AudioCompanionStreamStartMsg)];
  const size_t len = audio_companion_protocol_build_stream_start(buf, sizeof(buf), &params);
  if (bt_driver_audio_companion_notify_data(buf, len)) {
    PBL_LOG_INFO("Audio companion stream start sent (stream_id=%" PRIu32 ")", s_stream_id);
    s_need_stream_start = false;
  } else {
    s_send_backpressure_events++;
  }
}

static void prv_send_stream_stop_locked(uint8_t reason) {
  if (!s_stream_active) {
    return;
  }
  const AudioCompanionStreamStopMsg params = {
    .stream_id = s_stream_id,
    .reason = reason,
    .final_sequence = (s_next_sequence > 0) ? (s_next_sequence - 1) : 0,
    .final_sample_index = s_next_sample_index,
    .counters_crc_or_zero = 0,
  };
  uint8_t buf[sizeof(AudioCompanionStreamStopMsg)];
  const size_t len = audio_companion_protocol_build_stream_stop(buf, sizeof(buf), &params);
  if (prv_session_ready_locked()) {
    bt_driver_audio_companion_notify_data(buf, len);
  }
}

static bool prv_send_one_pending_gap_locked(void) {
  AudioCompanionSpoolPendingGap gap;
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  if (!audio_companion_spool_take_pending_gap(&gap)) {
    return true;
  }
  const AudioCompanionStreamGapMsg params = {
    .stream_id = s_stream_id,
    .first_missing_sequence = gap.first_missing_sequence,
    .missing_frame_count = gap.missing_frame_count,
    .first_missing_sample_index = gap.first_missing_sample_index,
    .reason = gap.reason,
    .watch_drop_counter = stats.dropped_overflow_frames,
  };
  uint8_t buf[sizeof(AudioCompanionStreamGapMsg)];
  const size_t len = audio_companion_protocol_build_stream_gap(buf, sizeof(buf), &params);
  if (!bt_driver_audio_companion_notify_data(buf, len)) {
    // Put it back so it is not lost; resend on the next drain.
    audio_companion_spool_record_gap(gap.first_missing_sequence, gap.missing_frame_count,
                                     gap.first_missing_sample_index, gap.reason);
    s_send_backpressure_events++;
    return false;
  }
  return true;
}

//! Flush every queued gap record, not one per slice. A gap notification is 26 bytes -- a tenth of
//! a data batch -- and holding records back is what let the pending table fill up in the first
//! place, which is where gap ranges used to lose their meaning. Bounded by the table size.
static bool prv_send_pending_gaps_locked(void) {
  for (uint32_t i = 0; i < AUDIO_COMPANION_MAX_PENDING_GAPS; i++) {
    if (!audio_companion_spool_has_pending_gap()) {
      return true;
    }
    if (!prv_send_one_pending_gap_locked()) {
      return false;
    }
  }
  return true;
}

static bool prv_send_data_batch_locked(void) {
  const uint16_t mtu = bt_driver_audio_companion_get_effective_mtu();
  if (mtu < sizeof(AudioCompanionStreamDataHeader) + 8) {
    return false;
  }
  const size_t max_message_bytes = (size_t)mtu - 3;

  // One stack buffer, not two: peek frames straight into the space after the header so this
  // runs in ~512 B on the 4 KB KernelBG stack (a second staging buffer + the NimBLE notify call
  // chain made the old frame uncomfortably deep).
  uint8_t message[512];
  const size_t header_size = sizeof(AudioCompanionStreamDataHeader);
  uint32_t first_sequence = 0;
  uint64_t first_sample_index = 0;
  uint8_t frame_count = 0;
  size_t payload_len = 0;

  const size_t budget =
      (max_message_bytes < sizeof(message)) ? max_message_bytes : sizeof(message);
  if (!audio_companion_spool_peek_batch(budget, header_size, &first_sequence,
                                        &first_sample_index, &frame_count, message + header_size,
                                        sizeof(message) - header_size, &payload_len)) {
    return false;
  }

  const size_t header_len = audio_companion_protocol_build_stream_data_header(
      message, header_size, s_stream_id, first_sequence, first_sample_index, frame_count, 0);
  // peek_batch reserved exactly header_size up front and wrote the payload right after it.
  PBL_ASSERTN(header_len == header_size && header_len + payload_len <= sizeof(message));

  if (!bt_driver_audio_companion_notify_data(message, header_len + payload_len)) {
    s_send_backpressure_events++;
    return false;
  }
  audio_companion_spool_mark_sent_through(first_sequence + frame_count - 1);
  s_sent_frames += frame_count;
  return true;
}

//! Bookkeeping shared by every "the receiver is not listening any more" path: a real BLE
//! disconnect, an unanswered liveness window, an unanswered silence probe.
//!
//! Rebasing the overflow baseline is what lets prv_mic_data_handler() tell "the spool is filling
//! because nobody is draining it" from drops that were already there, so it parks the mic exactly
//! once, when the spool actually saturates. Rewinding the unsent frames is what makes the audio
//! buffered during the outage survive: on reattach the stream is re-announced and those frames go
//! out ahead of anything new. Keeping the two together in one place is deliberate — they were
//! previously only on the disconnect path, and the liveness path's divergence from it is what
//! turned a suspended iOS app into a stopped microphone.
static void prv_note_receiver_gone_locked(void) {
  // Starts the offline clock; prv_note_receiver_activity_locked() stops it.
  if (s_offline_since_ms == 0) {
    s_offline_since_ms = prv_uptime_ms();
  }
  if (!s_stream_active) {
    return;
  }
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  s_offline_baseline_dropped = stats.dropped_overflow_frames;
  audio_companion_spool_rewind_unsent();
}

//! Stop TRANSMITTING when a streaming session has heard nothing from the receiver for too long.
//! Runs off the drain timer (active only while streaming). Catches the case where the receiver
//! vanished without the watch seeing a BLE disconnect, so the radio would otherwise talk forever
//! to nobody.
//!
//! It deliberately does NOT stop the microphone, and that distinction is the whole point.
//!
//! A silent receiver is not the same thing as an absent one. On iOS the app is regularly
//! suspended, jettisoned under memory pressure, or relaunched in the background, and in every one
//! of those cases it comes back within seconds to tens of seconds and reattaches to this stream.
//! This check used to call prv_stop_capture_locked() outright, so fifteen seconds of phone silence
//! threw away the microphone — and then the two ends deadlocked, because a suspended
//! bluetooth-central app is only ever woken BY the notifications that had just stopped. The user
//! saw a watch that recorded almost nothing unless they were holding the phone with the app open.
//!
//! The watch already knows how to survive a receiver that is not listening: an ordinary BLE
//! disconnect keeps capturing into the spool and parks the mic only once the spool starts dropping
//! (prv_park_capture_system_task_cb). A suspended receiver deserves exactly that treatment and no
//! worse, so do the same bookkeeping a disconnect does — rebase the overflow baseline, rewind the
//! unsent frames so they are resent on reattach — and let prv_reevaluate_locked() fall through to
//! the brief-disconnect bridge. Battery is still bounded: the drain timer stops immediately, and
//! the mic stops on its own once the spool saturates.
static void prv_check_receiver_liveness_locked(void) {
  if (s_state != AudioCompanionServiceStateStreaming || s_receiver_presumed_gone ||
      s_silence_suppressing) {
    return;
  }
  if ((prv_uptime_ms() - s_last_receiver_activity_ms) < RECEIVER_LIVENESS_TIMEOUT_MS) {
    return;
  }
  PBL_LOG_WRN("Audio companion: no receiver activity for %ums; spooling until it returns",
              (unsigned)RECEIVER_LIVENESS_TIMEOUT_MS);
  s_receiver_presumed_gone = true;
  prv_note_receiver_gone_locked();
  prv_reevaluate_locked();  // session no longer ready -> AuthorizedIdle, drain timer stops
}

//! AIMD pacing for the per-slice burst (see DRAIN_MAX_BATCHES_CATCH_UP). The transport refusing a
//! notification is the only honest signal that the link is at its limit, so treat it as the
//! decrease trigger and retreat all the way to the base burst.
static void prv_update_drain_burst_locked(bool refused, bool burst_cap_bound) {
  const uint32_t previous = s_drain_burst_cap;
  if (refused || !burst_cap_bound ||
      audio_companion_spool_frames_pending_send() < DRAIN_BURST_BACKLOG_FRAMES) {
    s_drain_burst_cap = DRAIN_MAX_BATCHES_PER_CALL;
  } else if (s_drain_burst_cap < DRAIN_MAX_BATCHES_CATCH_UP) {
    s_drain_burst_cap += DRAIN_BURST_STEP;
    if (s_drain_burst_cap > DRAIN_MAX_BATCHES_CATCH_UP) {
      s_drain_burst_cap = DRAIN_MAX_BATCHES_CATCH_UP;
    }
  }
  if (s_drain_burst_cap != previous) {
    PBL_LOG_DBG("Audio companion drain burst %u -> %u (backlog=%" PRIu32 "%s)",
                (unsigned)previous, (unsigned)s_drain_burst_cap,
                audio_companion_spool_frames_pending_send(), refused ? ", refused" : "");
  }
}

//! Sends at most s_drain_burst_cap notifications and returns. Anything still queued --
//! because the batch cap was hit or because we stopped on BLE backpressure -- is picked up by the
//! repeating drain timer, which is what paces us against the transport.
static void prv_drain_locked(void) {
  prv_check_receiver_liveness_locked();
  audio_companion_spool_apply_pressure_policy();
  if (!prv_session_ready_locked() || !s_stream_active) {
    return;
  }
  if (s_need_stream_start) {
    prv_send_stream_start_locked();
    if (s_need_stream_start) {
      prv_update_drain_burst_locked(true, false);
      return;  // backpressure; retry on next drain
    }
  }
  if (!prv_send_pending_gaps_locked()) {
    prv_update_drain_burst_locked(true, false);
    return;
  }
  const uint32_t burst_cap = s_drain_burst_cap;
  uint32_t batches_sent = 0;
  bool refused = false;
  while (audio_companion_spool_frames_pending_send() > 0) {
    if (!prv_send_data_batch_locked()) {
      refused = true;  // BLE backpressure: the drain timer will retry shortly
      break;
    }
    if (++batches_sent >= burst_cap) {
      break;  // Burst cap: the drain timer continues within DRAIN_PERIOD_MS.
    }
  }
  prv_update_drain_burst_locked(refused, batches_sent >= burst_cap);
  if (s_catch_up_burst && audio_companion_spool_frames_pending_send() == 0) {
    s_catch_up_burst = false;
    if (s_catch_up_timer != TIMER_INVALID_ID) {
      new_timer_stop(s_catch_up_timer);
    }
    prv_update_ble_responsiveness_locked();
  }
  if (s_silence_suppressing && audio_companion_spool_frames_pending_send() == 0 &&
      !audio_companion_spool_has_pending_gap()) {
    prv_stop_drain_timer_locked();
    // Stopping this timer also stops the only caller of prv_check_receiver_liveness_locked().
    // Hand that job to the probe rather than leaving the microphone unwatched for the whole
    // quiet, which on a real device is minutes or hours.
    if (!s_silence_probe_outstanding) {
      prv_arm_silence_probe_locked(SILENCE_PROBE_INTERVAL_MS);
    }
  }
}

static void prv_drain_system_task_cb(void *data) {
  pbl_mutex_lock(&s_drain_post_lock, PBL_FOREVER);
  s_drain_cb_pending = false;
  pbl_mutex_unlock(&s_drain_post_lock);

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_drain_locked();
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();  // the liveness check above can drop capture intent
  // Deliberately no re-post when frames remain: the repeating drain timer picks the backlog up
  // within DRAIN_PERIOD_MS. Re-posting here made the batch cap meaningless -- KernelBG looped
  // straight back into another burst -- which is what held the IPC ring full continuously.
}

//! Queue a drain on the system task, coalescing so at most one drain callback is ever outstanding.
//! The audio path posts a drain on (roughly) every push threshold and every drain-timer tick;
//! without coalescing those would pile up on the shared, 30-deep system-task queue and a transient
//! KernelBG stall could overflow it, which the OS treats as a fatal EventQueueFull reboot. Must be
//! called without s_lock held.
static void prv_post_drain_cb(void) {
  if (!s_initialized) {
    return;
  }
  pbl_mutex_lock(&s_drain_post_lock, PBL_FOREVER);
  const bool already_pending = s_drain_cb_pending;
  s_drain_cb_pending = true;
  pbl_mutex_unlock(&s_drain_post_lock);
  if (!already_pending) {
    system_task_add_callback(prv_drain_system_task_cb, NULL);
  }
}

static void prv_drain_timer_cb(void *data) {
  prv_post_drain_cb();
}

static void prv_start_drain_timer_locked(void) {
  // The drain timer calls the liveness check itself, so the probe has nothing left to cover.
  prv_stop_silence_probe_locked();
  if (s_drain_timer == TIMER_INVALID_ID) {
    s_drain_timer = new_timer_create();
  }
  new_timer_start(s_drain_timer, DRAIN_PERIOD_MS, prv_drain_timer_cb, NULL,
                  TIMER_START_FLAG_REPEATING);
}

static void prv_stop_drain_timer_locked(void) {
  if (s_drain_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_drain_timer);
  }
}

static void prv_stop_silence_probe_locked(void) {
  s_silence_probe_outstanding = false;
  if (s_silence_probe_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_silence_probe_timer);
  }
}

//! Runs on the system task: sends the probe, or acts on one that was never answered.
static void prv_silence_probe_system_task_cb(void *data) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_state != AudioCompanionServiceStateStreaming || !s_silence_suppressing ||
      !prv_session_ready_locked()) {
    // Conditions moved on under us (audio resumed, the session went away). Nothing to probe.
    prv_stop_silence_probe_locked();
  } else if (s_silence_probe_outstanding) {
    // We spoke to the receiver and it did not answer within the liveness window. Stop
    // transmitting and fall back to AuthorizedIdle, exactly as the drain-driven check does — and
    // for the same reason, WITHOUT stopping the microphone. An unanswered probe most often means
    // an iOS app that iOS has suspended or jettisoned, which comes back; the spool bridges the
    // gap and parks the mic on its own if it does not.
    PBL_LOG_WRN("Audio companion: silence probe unanswered; spooling until the receiver returns");
    s_silence_probe_outstanding = false;
    s_receiver_presumed_gone = true;
    prv_note_receiver_gone_locked();
    prv_reevaluate_locked();
  } else {
    PBL_LOG_DBG("Audio companion: probing the receiver during suppressed silence");
    s_silence_probe_outstanding = true;
    // Any control notification will do; the state push is the one that also tells a receiver
    // whose picture has gone stale what we are actually doing.
    prv_send_state_changed_locked();
    prv_arm_silence_probe_locked(RECEIVER_LIVENESS_TIMEOUT_MS);
  }
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();  // the branch above can drop capture intent
}

static void prv_silence_probe_timer_cb(void *data) {
  system_task_add_callback(prv_silence_probe_system_task_cb, NULL);
}

//! The way back out of a stationary mute.
//!
//! Alternates two states on one timer. While MUTED it waits POWER_SAVE_LISTEN_INTERVAL_MS and then
//! unmutes, which lets prv_reevaluate_locked() restart the microphone. While LISTENING it waits
//! POWER_SAVE_LISTEN_WINDOW_MS and then asks prv_room_is_quiet_locked() what it heard: quiet means
//! this really is a nightstand, so mute again; speech means the wrist simply was not moving, so
//! stay up and check again after the next window.
//!
//! Without this, a stationary mute could only end on a shake or a button press — so a conversation
//! that started while someone sat still was lost in its entirety, not merely delayed.
static void prv_power_save_listen_system_task_cb(void *data) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_runlevel != RunLevel_Stationary || !s_enabled) {
    prv_stop_power_save_listen_locked();
    pbl_mutex_unlock(&s_lock);
    prv_apply_pending();
    return;
  }
  if (s_stationary_muted) {
    // Empty the detector before listening: the verdict must come from THIS window's audio, not
    // from whatever was left in the ring when the microphone was switched off.
    prv_reset_silence_suppression_locked();
    PBL_LOG_DBG("Audio companion: power-save listen window opening");
    s_stationary_muted = false;
    prv_arm_power_save_listen_locked(POWER_SAVE_LISTEN_WINDOW_MS);
  } else {
    // The detector has had a whole window to form a view.
    prv_update_stationary_mute_locked();
    PBL_LOG_DBG("Audio companion: power-save listen window closed; muted=%u",
                (unsigned)s_stationary_muted);
  }
  prv_reevaluate_locked();
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

static void prv_power_save_listen_timer_cb(void *data) {
  system_task_add_callback(prv_power_save_listen_system_task_cb, NULL);
}

//! Retries a capture start that failed because the microphone was unavailable.
//!
//! Runs the ordinary state machine rather than poking the driver directly: prv_reevaluate_locked()
//! decides afresh whether capture is wanted (the user may have turned it off, the receiver may
//! have gone, the runlevel may have changed), and prv_apply_pending() does the start with s_lock
//! released, which is the only safe lock order.
static void prv_capture_retry_system_task_cb(void *data) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_state != AudioCompanionServiceStatePausedConflict || s_mic_conflict_active) {
    // Either we recovered by another route, or dictation is genuinely holding the mic and owns
    // the un-pause. Either way this retry has nothing to do.
    prv_stop_capture_retry_locked();
    pbl_mutex_unlock(&s_lock);
    return;
  }
  PBL_LOG_DBG("Audio companion: retrying capture after a mic conflict");
  prv_reevaluate_locked();
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

static void prv_capture_retry_timer_cb(void *data) {
  system_task_add_callback(prv_capture_retry_system_task_cb, NULL);
}

static void prv_arm_capture_retry_locked(void) {
  if (s_capture_retry_timer == TIMER_INVALID_ID) {
    s_capture_retry_timer = new_timer_create();
  }
  s_capture_retry_delay_ms = (s_capture_retry_delay_ms == 0)
                                 ? CAPTURE_RETRY_MIN_MS
                                 : MIN(s_capture_retry_delay_ms * 2, CAPTURE_RETRY_MAX_MS);
  new_timer_start(s_capture_retry_timer, s_capture_retry_delay_ms, prv_capture_retry_timer_cb,
                  NULL, 0);
}

static void prv_stop_capture_retry_locked(void) {
  s_capture_retry_delay_ms = 0;
  if (s_capture_retry_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_capture_retry_timer);
  }
}

static void prv_arm_power_save_listen_locked(uint32_t delay_ms) {
  if (s_power_save_listen_timer == TIMER_INVALID_ID) {
    s_power_save_listen_timer = new_timer_create();
  }
  new_timer_start(s_power_save_listen_timer, delay_ms, prv_power_save_listen_timer_cb, NULL, 0);
}

static void prv_stop_power_save_listen_locked(void) {
  if (s_power_save_listen_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_power_save_listen_timer);
  }
}

static void prv_arm_silence_probe_locked(uint32_t delay_ms) {
  if (s_silence_probe_timer == TIMER_INVALID_ID) {
    s_silence_probe_timer = new_timer_create();
  }
  new_timer_start(s_silence_probe_timer, delay_ms, prv_silence_probe_timer_cb, NULL, 0);
}

// ---- State machine ----

static void prv_begin_stream_locked(void) {
  uint32_t id = rand32();
  if (id == 0) {
    id = 1;
  }
  s_stream_id = id;
  s_next_sequence = 0;
  s_next_sample_index = 0;
  // Captured once here and resent verbatim on every re-announcement: a RESUME STREAM_START
  // must describe the same stream, and the receiver keys reattachment off a stable identity.
  s_stream_start_time_ms = prv_wall_clock_ms();
  s_stream_start_monotonic_ms = prv_uptime_ms();
  s_offline_baseline_dropped = 0;
  s_alert_baseline_dropped = 0;
  s_pending_resume_gap_reason = 0;  // a fresh stream carries no pending gap
  prv_reset_silence_suppression_locked();
  audio_companion_spool_reset();
  s_stream_active = true;
  s_need_stream_start = true;
  s_stream_resumed = false;  // a fresh stream begins at sequence 0
  prv_note_receiver_activity_locked();  // arm the liveness watchdog from stream start
}

//! Re-announce an already-running stream to a freshly (re)attached receiver. The receiver is a new
//! GATT session with no stream context, so it would silently drop resumed STREAM_DATA unless we
//! resend STREAM_START first. The RESUME flag tells it to take the first frame's sequence as the
//! contiguity base; rewinding makes every un-checkpointed frame resend, so buffered audio that
//! piled up while the receiver was away is delivered instead of lost.
static void prv_request_stream_reannounce_locked(void) {
  if (!s_stream_active) {
    return;
  }
  s_need_stream_start = true;
  s_stream_resumed = true;
  audio_companion_spool_rewind_unsent();
  prv_begin_catch_up_burst_locked();
}

static void prv_end_stream_locked(uint8_t stop_reason) {
  if (!s_stream_active) {
    return;
  }
  prv_record_silence_gap_locked();
  if (prv_session_ready_locked() && !s_need_stream_start) {
    // Flush all of them: the spool reset below discards whatever is left, so a record held back
    // here is loss the receiver never hears about.
    prv_send_pending_gaps_locked();
  }
  prv_send_stream_stop_locked(stop_reason);
  s_stream_active = false;
  s_need_stream_start = false;
  s_pending_resume_gap_reason = 0;
  prv_reset_silence_suppression_locked();
  audio_companion_spool_reset();
}

//! Single decision point: derives the externally visible state and the
//! capture/stream activity from the current inputs.
static void prv_reevaluate_locked(void) {
  if (!s_initialized) {
    return;
  }
  audio_companion_spool_apply_pressure_policy();

  // The current receiver session is no longer ready (disconnect, unsubscribe, or liveness
  // watchdog): the next ready transition must re-announce the stream to whatever attaches.
  if (!prv_session_ready_locked()) {
    s_stream_attached = false;
  }

  if (!s_enabled) {
    prv_stop_capture_locked();
    prv_end_stream_locked(AudioCompanionStopReasonUserDisabled);
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStateDisabled);
    return;
  }

  if (s_error) {
    prv_stop_capture_locked();
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStateError);
    return;
  }

  if (s_mic_conflict_active) {
    // Capture intent was already dropped in mic_conflict_begin; repeat it so the intent is
    // unambiguous however this branch was reached.
    prv_stop_capture_locked();
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStatePausedConflict);
    return;
  }

  if (s_low_battery) {
    if (s_owns_mic) {
      prv_record_silence_gap_locked();
      prv_begin_gap_pause_locked(AudioCompanionGapReasonLowBattery);
    }
    prv_stop_capture_locked();
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStatePausedLowBattery);
    return;
  }

  if (s_receiver_pause_requested) {
    if (s_owns_mic) {
      prv_record_silence_gap_locked();
      prv_begin_gap_pause_locked(AudioCompanionGapReasonUserDisabled);
    }
    prv_stop_capture_locked();
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStatePausedPolicy);
    return;
  }

  if (prv_power_save_active_locked()) {
    if (s_owns_mic) {
      prv_record_silence_gap_locked();
      prv_begin_gap_pause_locked(AudioCompanionGapReasonPowerSave);
    }
    prv_stop_capture_locked();
    prv_stop_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStatePausedPowerSave);
    return;
  }

  if (prv_session_ready_locked()) {
    if (!s_stream_active) {
      prv_begin_stream_locked();
    } else if (!s_stream_attached) {
      // A ready session is (re)attaching to an ongoing stream (reconnect or liveness revival).
      // The receiver is a fresh GATT session with no stream context, so re-announce STREAM_START
      // and resend buffered frames before any new data; finish the disconnect gap (if capture had
      // parked) so loss is reported exactly once.
      prv_request_stream_reannounce_locked();
      prv_finish_gap_pause_locked();
    } else {
      prv_finish_gap_pause_locked();
    }
    s_stream_attached = true;
    // Capture intent only; prv_apply_capture() takes the mic once s_lock is dropped, and parks on
    // PausedConflict if the driver is unavailable (dictation also fires the conflict hook).
    prv_start_capture_locked();
    prv_start_drain_timer_locked();
    prv_set_state_locked(AudioCompanionServiceStateStreaming);
    return;
  }

  // Enabled but no ready receiver session.
  prv_stop_drain_timer_locked();
  if (s_owns_mic && s_stream_active) {
    // Brief-disconnect bridge: keep capturing into the spool until overflow
    // (the mic handler parks capture when dropping starts).
    prv_set_state_locked(s_session.authorized
                             ? AudioCompanionServiceStateAuthorizedIdle
                             : AudioCompanionServiceStateIdle);
    return;
  }
  prv_record_silence_gap_locked();
  prv_stop_capture_locked();
  prv_set_state_locked(s_session.authorized ? AudioCompanionServiceStateAuthorizedIdle
                                            : AudioCompanionServiceStateIdle);
}

// ---- Consent flow ----

static void prv_consent_timeout_system_task_cb(void *data) {
  audio_companion_handle_consent_response(false);
}

static void prv_consent_timer_cb(void *data) {
  system_task_add_callback(prv_consent_timeout_system_task_cb, NULL);
}

static void prv_enable_timeout_system_task_cb(void *data) {
  audio_companion_handle_enable_response(false);
}

static void prv_enable_timer_cb(void *data) {
  system_task_add_callback(prv_enable_timeout_system_task_cb, NULL);
}

static void prv_handle_auth_request_locked(const AudioCompanionAuthRequest *req) {
  const uint8_t granted = (req->proto_version < AUDIO_COMPANION_PROTOCOL_VERSION)
                              ? req->proto_version
                              : AUDIO_COMPANION_PROTOCOL_VERSION;
  if (!s_enabled) {
    prv_send_auth_result_locked(req->request_token, AudioCompanionAuthStatusDeniedDisabled, 0);
    return;
  }

  switch (audio_companion_auth_evaluate(req->receiver_id)) {
    case AudioCompanionAuthEvalMatch:
      PBL_LOG_INFO("Audio companion auth: receiver match, session authorized");
      s_session.authorized = true;
      memcpy(s_session.receiver_id, req->receiver_id, sizeof(s_session.receiver_id));
      s_session.granted_proto_version = granted;
      // An AUTH_REQUEST is by definition a receiver session with NO stream context, so whatever
      // we announced to the previous one does not count. Detaching here is what makes
      // prv_reevaluate_locked() re-announce STREAM_START below.
      s_stream_attached = false;
      prv_send_auth_result_locked(req->request_token, AudioCompanionAuthStatusOk, granted);
      prv_reevaluate_locked();
      break;
    case AudioCompanionAuthEvalMismatch:
      PBL_LOG_WRN("Audio companion auth denied: receiver mismatch");
      prv_send_auth_result_locked(req->request_token, AudioCompanionAuthStatusDeniedMismatch,
                                  0);
      break;
    case AudioCompanionAuthEvalNoReceiver:
      if (s_consent.pending) {
        // A receiver re-authorizing mid-consent is the SAME receiver on a NEW GATT session -- an
        // app relaunch, or a resync on an ACL the watch never saw drop, either of which happens
        // easily inside the 60 s a person takes to answer the prompt. The consent answer is
        // addressed by request token, so unless we adopt the new one the eventual AUTH_RESULT
        // names a token the app has already forgotten: the app drops the reply, never authorizes,
        // and both sides wait forever. Nothing clears it either -- the consent state is cleared
        // only by a BLE disconnect, which by construction did not happen.
        if (memcmp(req->receiver_id, s_consent.receiver_id,
                   AUDIO_COMPANION_RECEIVER_ID_BYTES) == 0) {
          s_consent.request_token = req->request_token;
          s_consent.proto_version = req->proto_version;
          strncpy(s_consent.name, req->name, AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES);
        }
        prv_send_auth_result_locked(req->request_token,
                                    AudioCompanionAuthStatusPendingUserConsent, 0);
        break;
      }
      if (!s_consent_handler) {
        PBL_LOG_WRN("Audio companion auth denied: no consent handler registered");
        prv_send_auth_result_locked(req->request_token, AudioCompanionAuthStatusInvalid, 0);
        break;
      }
      s_consent = (ConsentRequest){
        .pending = true,
        .request_token = req->request_token,
        .proto_version = req->proto_version,
      };
      memcpy(s_consent.receiver_id, req->receiver_id, sizeof(s_consent.receiver_id));
      strncpy(s_consent.name, req->name, AUDIO_COMPANION_MAX_RECEIVER_NAME_BYTES);
      prv_send_auth_result_locked(req->request_token,
                                  AudioCompanionAuthStatusPendingUserConsent, 0);
      if (s_consent_timer == TIMER_INVALID_ID) {
        s_consent_timer = new_timer_create();
      }
      new_timer_start(s_consent_timer, CONSENT_TIMEOUT_MS, prv_consent_timer_cb, NULL, 0);
      PBL_LOG_INFO("Audio companion auth: no receiver bound; requesting user consent");
      // NOTE: invoked on the system task with s_lock held. Handlers must defer
      // any UI work to KernelMain and must not call back into this service
      // synchronously.
      s_consent_handler(s_consent.name);
      break;
  }
}

void audio_companion_handle_consent_response(bool granted) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (!s_consent.pending) {
    pbl_mutex_unlock(&s_lock);
    return;
  }
  s_consent.pending = false;
  if (s_consent_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_consent_timer);
  }
  PBL_LOG_INFO("Audio companion consent response: %s", granted ? "granted" : "declined");

  const uint8_t token = s_consent.request_token;
  if (granted && audio_companion_auth_store_receiver(s_consent.receiver_id, s_consent.name)) {
    const uint8_t granted_version =
        (s_consent.proto_version < AUDIO_COMPANION_PROTOCOL_VERSION)
            ? s_consent.proto_version
            : AUDIO_COMPANION_PROTOCOL_VERSION;
    s_session.authorized = true;
    memcpy(s_session.receiver_id, s_consent.receiver_id, sizeof(s_session.receiver_id));
    s_session.granted_proto_version = granted_version;
    s_stream_attached = false;  // fresh receiver session; see prv_handle_auth_request_locked
    prv_send_auth_result_locked(token, AudioCompanionAuthStatusOk, granted_version);
    prv_reevaluate_locked();
  } else {
    prv_send_auth_result_locked(token, AudioCompanionAuthStatusInvalid, 0);
  }
  memset(&s_consent, 0, sizeof(s_consent));
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

void audio_companion_set_consent_handler(AudioCompanionConsentHandler handler) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_consent_handler = handler;
  pbl_mutex_unlock(&s_lock);
}

static void prv_handle_enable_request_locked(const AudioCompanionEnableRequestMsg *req) {
  if (s_enabled) {
    prv_send_ack_locked(req->request_token, AudioCompanionAckStatusOk);
    return;
  }
  if (s_enable_request.pending) {
    prv_send_ack_locked(req->request_token, AudioCompanionAckStatusBadState);
    return;
  }
  if (!s_enable_handler) {
    prv_send_ack_locked(req->request_token, AudioCompanionAckStatusRejected);
    return;
  }

  s_enable_request = (EnableRequest) {
    .pending = true,
    .request_token = req->request_token,
  };
  if (s_enable_timer == TIMER_INVALID_ID) {
    s_enable_timer = new_timer_create();
  }
  new_timer_start(s_enable_timer, CONSENT_TIMEOUT_MS, prv_enable_timer_cb, NULL, 0);
  PBL_LOG_INFO("Audio companion: requesting user approval to enable background audio");
  s_enable_handler();
}

void audio_companion_handle_enable_response(bool granted) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (!s_enable_request.pending) {
    pbl_mutex_unlock(&s_lock);
    return;
  }
  const uint8_t token = s_enable_request.request_token;
  s_enable_request.pending = false;
  if (s_enable_timer != TIMER_INVALID_ID) {
    new_timer_stop(s_enable_timer);
  }
  PBL_LOG_INFO("Audio companion enable response: %s", granted ? "granted" : "declined");
  if (granted) {
    shell_prefs_set_audio_companion_enabled(true);
    if (!s_enabled) {
      s_enabled = true;
      prv_reevaluate_locked();
    }
    prv_send_ack_locked(token, AudioCompanionAckStatusOk);
  } else {
    prv_send_ack_locked(token, AudioCompanionAckStatusRejected);
  }
  memset(&s_enable_request, 0, sizeof(s_enable_request));
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

void audio_companion_set_enable_handler(AudioCompanionEnableHandler handler) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_enable_handler = handler;
  pbl_mutex_unlock(&s_lock);
}

// ---- Control message handling ----

static void prv_handle_checkpoint_locked(const AudioCompanionCheckpointMsg *checkpoint) {
  if (!s_session.authorized) {
    prv_send_error_locked(AudioCompanionErrorCodeUnauthorized, 0);
    return;
  }
  if (!s_stream_active || checkpoint->stream_id != s_stream_id) {
    prv_send_ack_locked(checkpoint->request_token, AudioCompanionAckStatusRejected);
    return;
  }
  audio_companion_spool_trim_through(checkpoint->highest_contiguous_sequence_persisted);
  audio_companion_spool_apply_pressure_policy();

  const bool pause_requested =
      (checkpoint->receiver_flags & (AUDIO_COMPANION_RECEIVER_FLAG_PAUSE_REQUESTED |
                                     AUDIO_COMPANION_RECEIVER_FLAG_LOW_STORAGE)) != 0;
  prv_send_ack_locked(checkpoint->request_token, AudioCompanionAckStatusOk);
  if (pause_requested != s_receiver_pause_requested) {
    s_receiver_pause_requested = pause_requested;
    prv_reevaluate_locked();
  }
}

static void prv_handle_control_msg_locked(const AudioCompanionControlMsg *msg) {
  switch (msg->msg_id) {
    case AudioCompanionCtrlMsgIdAuthRequest:
      prv_handle_auth_request_locked(&msg->auth_request);
      break;
    case AudioCompanionCtrlMsgIdAuthRevoke:
      if (s_session.authorized &&
          memcmp(msg->auth_revoke.receiver_id, s_session.receiver_id,
                 AUDIO_COMPANION_RECEIVER_ID_BYTES) == 0) {
        audio_companion_auth_forget_receiver();
        prv_end_stream_locked(AudioCompanionStopReasonPolicy);
        s_session.authorized = false;
        prv_send_revoked_locked(AudioCompanionRevokedReasonAppRequested);
        prv_reevaluate_locked();
      } else {
        prv_send_error_locked(AudioCompanionErrorCodeUnauthorized, 0);
      }
      break;
    case AudioCompanionCtrlMsgIdCheckpoint:
      prv_handle_checkpoint_locked(&msg->checkpoint);
      break;
    case AudioCompanionCtrlMsgIdPauseRequest:
      if (!s_session.authorized) {
        prv_send_error_locked(AudioCompanionErrorCodeUnauthorized, 0);
        break;
      }
      prv_send_ack_locked(msg->pause_request.request_token, AudioCompanionAckStatusOk);
      if (msg->pause_request.reason == AudioCompanionPauseReasonUser) {
        // Explicit user stop: end the stream so the next resume opens a fresh segment instead
        // of replaying the pre-stop spool. Policy/low-storage pauses keep the stream so a
        // transient pause resumes the same recording.
        prv_end_stream_locked(AudioCompanionStopReasonUserDisabled);
      }
      s_receiver_pause_requested = true;
      prv_reevaluate_locked();
      break;
    case AudioCompanionCtrlMsgIdResumeRequest:
      if (!s_session.authorized) {
        prv_send_error_locked(AudioCompanionErrorCodeUnauthorized, 0);
        break;
      }
      prv_send_ack_locked(msg->resume_request.request_token, AudioCompanionAckStatusOk);
      if (s_receiver_pause_requested) {
        s_receiver_pause_requested = false;
        prv_reevaluate_locked();
      }
      break;
    case AudioCompanionCtrlMsgIdReceiverHealth:
      if (!s_session.authorized) {
        prv_send_error_locked(AudioCompanionErrorCodeUnauthorized, 0);
        break;
      }
      PBL_LOG_DBG("Audio receiver health: battery %u pct, app state %u, queue %" PRIu32,
                  msg->receiver_health.battery_pct, msg->receiver_health.app_state,
                  msg->receiver_health.queue_depth_frames);
      prv_send_ack_locked(msg->receiver_health.request_token, AudioCompanionAckStatusOk);
      break;
    case AudioCompanionCtrlMsgIdEnableRequest:
      prv_handle_enable_request_locked(&msg->enable_request);
      break;
    default:
      break;
  }
}

// ---- bt_driver entry points (BT host task -> system task handoff) ----

typedef struct {
  size_t length;
  uint8_t data[];
} ControlWriteWork;

static void prv_control_write_system_task_cb(void *data) {
  ControlWriteWork *work = data;
  AudioCompanionControlMsg msg;
  const AudioCompanionParseResult result =
      audio_companion_protocol_parse_control(work->data, work->length, &msg);

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  switch (result) {
    case AudioCompanionParseResultOk: {
      const bool was_presumed_gone = s_receiver_presumed_gone;
      prv_note_receiver_activity_locked();
      prv_handle_control_msg_locked(&msg);
      if (was_presumed_gone) {
        // Receiver came back after the liveness watchdog tripped; resume streaming.
        prv_reevaluate_locked();
      }
      break;
    }
    case AudioCompanionParseResultMalformed:
      PBL_LOG_WRN("Audio companion: malformed control write (%u bytes)",
                  (unsigned)work->length);
      prv_send_error_locked(AudioCompanionErrorCodeMalformed, 0);
      break;
    case AudioCompanionParseResultUnknown:
      break;
  }
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
  kernel_free(work);
}

void audio_companion_handle_control_write(const uint8_t *data, size_t length) {
  if (!s_initialized || !data || length == 0 || length > 128) {
    return;
  }
  ControlWriteWork *work = kernel_malloc(sizeof(ControlWriteWork) + length);
  if (!work) {
    return;
  }
  work->length = length;
  memcpy(work->data, data, length);
  system_task_add_callback(prv_control_write_system_task_cb, work);
}

static void prv_subscription_system_task_cb(void *data) {
  const uintptr_t packed = (uintptr_t)data;
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_session.data_subscribed = (packed & 1) != 0;
  s_session.control_subscribed = (packed & 2) != 0;
  PBL_LOG_INFO("Audio companion subscriptions: data=%u control=%u",
               (unsigned)s_session.data_subscribed, (unsigned)s_session.control_subscribed);
  if (s_session.data_subscribed || s_session.control_subscribed) {
    prv_note_receiver_activity_locked();  // a (re)subscribe is proof the receiver is present
  }
  if (prv_session_ready_locked()) {
    s_offline_baseline_dropped = 0;
  }
  // Re-announce on reconnect is centralized in prv_reevaluate_locked (keyed off s_stream_attached)
  // so it fires regardless of whether the receiver subscribes before or after AUTH — iOS subscribes
  // both characteristics first, so doing it here would miss the common reconnect.
  prv_reevaluate_locked();
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

void audio_companion_handle_subscription_change(bool data_subscribed,
                                                bool control_subscribed) {
  if (!s_initialized) {
    return;
  }
  const uintptr_t packed = (data_subscribed ? 1 : 0) | (control_subscribed ? 2 : 0);
  system_task_add_callback(prv_subscription_system_task_cb, (void *)packed);
}

static void prv_disconnect_system_task_cb(void *data) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  PBL_LOG_INFO("Audio companion: receiver disconnected; session reset");
  s_session.authorized = false;
  s_session.data_subscribed = false;
  s_session.control_subscribed = false;
  memset(s_session.receiver_id, 0, sizeof(s_session.receiver_id));
  // The policy pause is the receiver's session-scoped request; clear it so the watch is never
  // stranded in PausedPolicy once the receiver is gone. The phone re-asserts pause on reconnect
  // if it still wants one (declarative reconcile on the app side).
  s_receiver_pause_requested = false;
  s_receiver_presumed_gone = false;
  if (s_consent.pending) {
    s_consent.pending = false;
    if (s_consent_timer != TIMER_INVALID_ID) {
      new_timer_stop(s_consent_timer);
    }
    memset(&s_consent, 0, sizeof(s_consent));
  }
  prv_note_receiver_gone_locked();
  prv_reevaluate_locked();
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

void audio_companion_handle_disconnect(void) {
  if (!s_initialized) {
    return;
  }
  system_task_add_callback(prv_disconnect_system_task_cb, NULL);
}

void audio_companion_fill_info(uint8_t *buf, size_t *length_in_out) {
  if (!buf || !length_in_out) {
    return;
  }
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  uint8_t flags = AUDIO_COMPANION_INFO_FLAG_BACKPRESSURE_COUNTER;
  if (audio_companion_auth_receiver_exists()) {
    flags |= AUDIO_COMPANION_INFO_FLAG_RECEIVER_BOUND;
  }
  if (s_enabled) {
    flags |= AUDIO_COMPANION_INFO_FLAG_ENABLED;
  }
  if (s_consent.pending) {
    flags |= AUDIO_COMPANION_INFO_FLAG_CONSENT_PENDING;
  }
  const AudioCompanionInfo info = {
    .info_version = 1,
    .protocol_min = AUDIO_COMPANION_PROTOCOL_VERSION,
    .protocol_max = AUDIO_COMPANION_PROTOCOL_VERSION,
    .service_state = (uint8_t)s_state,
    .codec_bitmap = 0x01,  // Speex wideband
    .flags = flags,
    .send_backpressure_events = s_send_backpressure_events,
  };
  *length_in_out = audio_companion_protocol_build_info(buf, *length_in_out, &info);
  pbl_mutex_unlock(&s_lock);
  PBL_LOG_INFO("Audio companion info read (state=%u flags=0x%02x)", info.service_state,
               info.flags);
}

// ---- Mic arbitration (called from the voice service) ----

void audio_companion_mic_conflict_begin(void) {
  if (!s_initialized) {
    return;
  }
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_owns_mic) {
    prv_record_silence_gap_locked();
    prv_begin_gap_pause_locked(AudioCompanionGapReasonMicConflict);
    s_mic_conflicts++;
  }
  prv_stop_capture_locked();
  s_mic_conflict_active = true;
  prv_reevaluate_locked();
  pbl_mutex_unlock(&s_lock);
  // Synchronous: the voice service calls this immediately before claiming the mic itself, so the
  // driver must actually be released before we return.
  prv_apply_pending();
}

void audio_companion_mic_conflict_end(void) {
  if (!s_initialized) {
    return;
  }
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_mic_conflict_active = false;
  prv_reevaluate_locked();
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

// ---- Battery policy ----

static void prv_battery_event_handler(PebbleEvent *event, void *context) {
  const BatteryChargeState charge = battery_get_charge_state();
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  bool low = s_low_battery;
  if (charge.charge_percent < CONFIG_AUDIO_COMPANION_LOW_BATTERY_PERCENT) {
    low = true;
  } else if (charge.charge_percent >=
             CONFIG_AUDIO_COMPANION_LOW_BATTERY_PERCENT + LOW_BATTERY_RESUME_HYSTERESIS_PCT) {
    low = false;
  }
  if (low != s_low_battery) {
    s_low_battery = low;
    prv_reevaluate_locked();
  }
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

// ---- Public API ----

//! Record why the previous session ended into the persisted reboot ring, so a user who notices
//! the watch restarted can read the fault class from Settings. The OS captured the reason in a
//! battery-backed register at boot; we just persist it alongside whether background audio was on.
//! Does its own flash I/O, so it must run without the service lock held.
static void prv_record_boot_reboot_trace(void) {
  RebootReason full;
  reboot_reason_get_last_reboot_reason_full(&full);
  const uint8_t reason = (uint8_t)reboot_reason_get_last_reboot_reason();
  const bool enabled = shell_prefs_get_audio_companion_enabled();

  // Carry the OS's stuck-task evidence into the persisted ring. Without this the trace can only
  // say "watchdog", which is the fault class, not the culprit; with it the watch can name the
  // task that stalled and hand over a PC/LR to symbolize against the firmware ELF.
  AudioCompanionRebootTraceDetail detail = {0};
  if (reason == RebootReasonCode_Watchdog) {
    detail.watchdog_bits = full.data8[0];
    detail.watchdog_mask = full.data8[1];
    detail.fault_pc = full.watchdog.stuck_task_pc;
    detail.fault_lr = full.watchdog.stuck_task_lr;
    detail.fault_extra = full.watchdog.stuck_task_callback;
  } else if (reason == RebootReasonCode_EventQueueFull) {
    detail.fault_pc = full.event_queue.push_lr;
    detail.fault_lr = full.event_queue.current_event;
    detail.fault_extra = full.event_queue.dropped_event;
  } else if (reason == RebootReasonCode_OutOfMemory) {
    detail.fault_pc = full.heap_data.heap_alloc_lr;
    detail.fault_lr = full.heap_data.heap_ptr;
  } else if (audio_companion_reboot_trace_is_error_reason(reason)) {
    detail.fault_pc = full.extra.value;
  }

  AudioCompanionRebootTrace trace;
  audio_companion_reboot_trace_load(&trace);
  audio_companion_reboot_trace_record(&trace, reason, enabled, (uint32_t)rtc_get_time(), &detail);

  // Fail closed on a crash run. The bootloader escalates to recovery firmware when the watch keeps
  // restarting before it stabilizes, which costs the user a reflash to get their watch back --
  // a background feature must not be able to cause that. Stand down and say so instead; the user
  // re-enables when they choose to.
  const bool stand_down =
      enabled && trace.consecutive_fault_boots >= AUDIO_COMPANION_FAULT_LOOP_THRESHOLD;
  if (stand_down) {
    // Clear the run so re-enabling gets a fresh start rather than tripping again next boot.
    trace.consecutive_fault_boots = 0;
    PBL_LOG_WRN("Audio companion: %u restarts without a healthy session; disabling background "
                "audio so the watch does not fall back to recovery",
                (unsigned)AUDIO_COMPANION_FAULT_LOOP_THRESHOLD);
    shell_prefs_set_audio_companion_enabled(false);
  }

  audio_companion_reboot_trace_save(&trace);

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_reboot_trace = trace;
  pbl_mutex_unlock(&s_lock);

  if (stand_down) {
    audio_companion_apply_enabled(false);
    prv_post_stand_down_alert();
  }

  if (audio_companion_reboot_trace_is_error_reason(reason)) {
    const AudioCompanionRebootTraceEntry *newest = audio_companion_reboot_trace_newest(&trace);
    const char *stuck = audio_companion_reboot_trace_stuck_task_name(newest);
    // Hashed logging allows at most two %s per line, so fold the missing-task case into one.
    PBL_LOG_WRN("Audio companion: previous restart was a fault (%s, stuck task %s); "
                "%u faults / %u boots logged",
                audio_companion_reboot_trace_reason_name(reason), stuck ? stuck : "n/a",
                (unsigned)trace.total_error_reboots, (unsigned)trace.total_reboots);
    PBL_LOG_WRN("Audio companion: fault detail pc=0x%08" PRIx32 " lr=0x%08" PRIx32
                " extra=0x%08" PRIx32 " (audio %s)",
                detail.fault_pc, detail.fault_lr, detail.fault_extra,
                enabled ? "enabled" : "disabled");
  } else {
    PBL_LOG_INFO("Audio companion: previous restart reason: %s",
                 audio_companion_reboot_trace_reason_name(reason));
  }
}

void audio_companion_init(void) {
  audio_companion_spool_init();
  audio_companion_auth_init();

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  audio_companion_reboot_trace_clear(&s_reboot_trace);
  s_initialized = true;
  s_enabled = shell_prefs_get_audio_companion_enabled();
  s_pause_stationary_enabled =
      shell_prefs_get_audio_companion_pause_stationary_enabled();
  s_pause_low_power_enabled =
      shell_prefs_get_audio_companion_pause_low_power_enabled();
  s_silence_mode =
      (AudioCompanionSilenceMode)shell_prefs_get_audio_companion_silence_mode();
  const BatteryChargeState charge = battery_get_charge_state();
  s_low_battery = charge.charge_percent < CONFIG_AUDIO_COMPANION_LOW_BATTERY_PERCENT;
  prv_reevaluate_locked();
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();

  s_battery_event_info = (EventServiceInfo){
    .type = PEBBLE_BATTERY_STATE_CHANGE_EVENT,
    .handler = prv_battery_event_handler,
  };
  event_service_client_subscribe(&s_battery_event_info);
}

void audio_companion_handle_prefs_loaded(void) {
  // The settings live in the shell prefs file, which shell_prefs_init() loads only after this
  // service has already initialized at boot. So audio_companion_init() above necessarily reads
  // compile-time defaults. Re-read the settings here, once the prefs are actually loaded, so a
  // reboot (which connection resets can trigger) restores the user's choices instead of defaults.
  if (!s_initialized) {
    return;
  }
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  s_enabled = shell_prefs_get_audio_companion_enabled();
  s_pause_stationary_enabled = shell_prefs_get_audio_companion_pause_stationary_enabled();
  s_pause_low_power_enabled = shell_prefs_get_audio_companion_pause_low_power_enabled();
  s_silence_mode =
      (AudioCompanionSilenceMode)shell_prefs_get_audio_companion_silence_mode();
  prv_reevaluate_locked();
  const bool record_reboot = !s_reboot_trace_recorded;
  s_reboot_trace_recorded = true;
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();

  // Now that the persisted prefs are loaded (so the "enabled" flag is accurate) capture the
  // previous reboot reason into the flight recorder. Done once per boot, outside the lock.
  if (record_reboot) {
    prv_record_boot_reboot_trace();
  }
}

void audio_companion_get_reboot_trace(struct AudioCompanionRebootTrace *out) {
  if (!out) {
    return;
  }
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  *out = s_reboot_trace;
  pbl_mutex_unlock(&s_lock);
}

bool audio_companion_is_enabled(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const bool enabled = s_enabled;
  pbl_mutex_unlock(&s_lock);
  return enabled;
}

void audio_companion_apply_enabled(bool enabled) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_enabled != enabled) {
    s_enabled = enabled;
    prv_reevaluate_locked();
  }
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

void audio_companion_set_enabled(bool enabled) {
  shell_prefs_set_audio_companion_enabled(enabled);
  audio_companion_apply_enabled(enabled);
}

void audio_companion_set_runlevel(RunLevel runlevel) {
  if (!s_initialized) {
    return;
  }
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_runlevel != runlevel) {
    s_runlevel = runlevel;
    // Entering or leaving Stationary decides the mute and owns the listen cadence.
    prv_update_stationary_mute_locked();
    prv_reevaluate_locked();
  }
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

bool audio_companion_get_pause_stationary_enabled(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const bool enabled = s_pause_stationary_enabled;
  pbl_mutex_unlock(&s_lock);
  return enabled;
}

void audio_companion_set_pause_stationary_enabled(bool enabled) {
  shell_prefs_set_audio_companion_pause_stationary_enabled(enabled);
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_pause_stationary_enabled != enabled) {
    s_pause_stationary_enabled = enabled;
    prv_reevaluate_locked();
  }
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

bool audio_companion_get_pause_low_power_enabled(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const bool enabled = s_pause_low_power_enabled;
  pbl_mutex_unlock(&s_lock);
  return enabled;
}

void audio_companion_set_pause_low_power_enabled(bool enabled) {
  shell_prefs_set_audio_companion_pause_low_power_enabled(enabled);
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_pause_low_power_enabled != enabled) {
    s_pause_low_power_enabled = enabled;
    prv_reevaluate_locked();
  }
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

bool audio_companion_get_silence_suppression_enabled(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const bool enabled = s_silence_mode != AudioCompanionSilenceModeOff;
  pbl_mutex_unlock(&s_lock);
  return enabled;
}

void audio_companion_set_silence_suppression_enabled(bool enabled) {
  shell_prefs_set_audio_companion_silence_suppression_enabled(enabled);
  audio_companion_set_silence_mode(enabled ? AudioCompanionSilenceModeLight
                                           : AudioCompanionSilenceModeOff);
}

AudioCompanionSilenceMode audio_companion_get_silence_mode(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const AudioCompanionSilenceMode mode = s_silence_mode;
  pbl_mutex_unlock(&s_lock);
  return mode;
}

void audio_companion_set_silence_mode(AudioCompanionSilenceMode mode) {
  if (mode >= AudioCompanionSilenceModeCount) {
    return;
  }
  shell_prefs_set_audio_companion_silence_mode((uint8_t)mode);
  shell_prefs_set_audio_companion_silence_suppression_enabled(
      mode != AudioCompanionSilenceModeOff);
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_silence_mode != mode) {
    // BUG: this used to end the run only when switching TO Off, so Light -> Aggressive left a
    // suppression run and a half-full trailing window in place while the rule under them changed
    // — the window's frame count and quiet population now mean something different, and the head
    // index can be past the end of the new window. Any mode change ends the run and re-arms from
    // scratch, which is also the honest reading: the user changed the rule, so the old verdict
    // stops applying.
    prv_record_silence_gap_locked();
    // ...and the trailing window with it. Its frame count and quiet population were collected
    // against the old mode's threshold and length; read under the new one they mean something
    // else, and a shorter window would leave the ring head past its own end.
    prv_silence_window_reset_locked();
    if (prv_session_ready_locked()) {
      prv_start_drain_timer_locked();
    }
    s_silence_mode = mode;
  }
  pbl_mutex_unlock(&s_lock);
}

AudioCompanionServiceState audio_companion_get_state(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const AudioCompanionServiceState state = s_state;
  pbl_mutex_unlock(&s_lock);
  return state;
}

void audio_companion_get_diagnostics(AudioCompanionDiagnostics *diag_out) {
  if (!diag_out) {
    return;
  }
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  AudioCompanionSpoolStats stats;
  audio_companion_spool_get_stats(&stats);
  const SilenceModeConfig *silence_config = prv_silence_mode_config(s_silence_mode);
  *diag_out = (AudioCompanionDiagnostics){
    .state = s_state,
    .captured_frames = s_captured_frames,
    .sent_frames = s_sent_frames,
    .send_backpressure_events = s_send_backpressure_events,
    .gap_records = stats.gap_records,
    .dropped_overflow_frames = stats.dropped_overflow_frames,
    .mic_conflicts = s_mic_conflicts,
    .suppressed_silence_frames = s_suppressed_silence_frames,
    .silence_runs = s_silence_runs,
    .silence_level = s_silence_last_level,
    .silence_enter_threshold = silence_config ? silence_config->quiet_threshold : 0,
    .silence_resume_threshold = silence_config ? silence_config->resume_threshold : 0,
    .silence_raw_level = s_silence_last_raw_level,
    .silence_quiet_permille = prv_silence_window_permille_locked(),
    .silence_suppressing = s_silence_suppressing,
    .spool_bytes = stats.current_bytes,
    .spool_high_water_bytes = stats.high_water_bytes,
    .loss_alerts_posted = s_loss_alerts_posted,
    .kernel_heap_free_bytes = audio_companion_spool_heap_free_bytes(),
  };
  pbl_mutex_unlock(&s_lock);
}

bool audio_companion_get_receiver_name(char *buf, size_t buf_size) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  const bool result = audio_companion_auth_get_receiver_name(buf, buf_size);
  pbl_mutex_unlock(&s_lock);
  return result;
}

void audio_companion_forget_receiver(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  audio_companion_auth_forget_receiver();
  prv_end_stream_locked(AudioCompanionStopReasonPolicy);
  if (s_session.authorized) {
    s_session.authorized = false;
    prv_send_revoked_locked(AudioCompanionRevokedReasonUserOnWatch);
  }
  memset(s_session.receiver_id, 0, sizeof(s_session.receiver_id));
  prv_reevaluate_locked();
  pbl_mutex_unlock(&s_lock);
  prv_apply_pending();
}

#ifdef UNITTEST
//! Lets the mic fakes assert that the driver is never entered while the service lock is held.
//! That ordering is the deadlock: the driver calls prv_mic_data_handler() under its own mutex,
//! and the handler takes s_lock.
struct pbl_mutex *audio_companion_test_get_lock(void) { return &s_lock; }

//! Re-arm the once-per-boot reboot-trace capture so a test can simulate consecutive boots.
void audio_companion_test_force_reboot_trace_capture(void) { s_reboot_trace_recorded = false; }

//! The suppressed-silence receiver probe's timer, so a test can fire it deterministically.
TimerID audio_companion_test_get_silence_probe_timer(void) { return s_silence_probe_timer; }

TimerID audio_companion_test_get_power_save_listen_timer(void) { return s_power_save_listen_timer; }

TimerID audio_companion_test_get_capture_retry_timer(void) { return s_capture_retry_timer; }

void audio_companion_test_reset(void) {
  audio_companion_spool_reset();
  pbl_mutex_init(&s_lock);
  pbl_mutex_init(&s_capture_lock);
  pbl_mutex_init(&s_drain_post_lock);
  s_initialized = false;
  s_state = AudioCompanionServiceStateDisabled;
  s_enabled = false;
  memset(&s_session, 0, sizeof(s_session));
  memset(&s_consent, 0, sizeof(s_consent));
  memset(&s_enable_request, 0, sizeof(s_enable_request));
  s_consent_handler = NULL;
  s_enable_handler = NULL;
  s_consent_timer = TIMER_INVALID_ID;
  s_enable_timer = TIMER_INVALID_ID;
  s_owns_mic = false;
  s_capture_wanted = false;
  s_mic_started = false;
  s_response_state_desired = ResponseTimeMax;
  s_response_period_desired = 0;
  s_response_dirty = false;
  s_stream_active = false;
  s_need_stream_start = false;
  s_stream_resumed = false;
  s_stream_attached = false;
  s_stream_id = 0;
  s_next_sequence = 0;
  s_next_sample_index = 0;
  s_drain_timer = TIMER_INVALID_ID;
  s_catch_up_timer = TIMER_INVALID_ID;
  s_catch_up_burst = false;
  s_drain_burst_cap = DRAIN_MAX_BATCHES_PER_CALL;
  s_drain_cb_pending = false;
  s_capture_parked = false;
  s_pause_started_ms = 0;
  s_pending_resume_gap_reason = 0;
  s_last_receiver_activity_ms = 0;
  s_offline_since_ms = 0;
  s_receiver_presumed_gone = false;
  s_silence_probe_timer = TIMER_INVALID_ID;
  s_silence_probe_outstanding = false;
  // These four leaked between test cases: a mute or a pending retry left over from the previous
  // case changed what the next one measured.
  s_power_save_listen_timer = TIMER_INVALID_ID;
  s_capture_retry_timer = TIMER_INVALID_ID;
  s_capture_retry_delay_ms = 0;
  s_stationary_muted = false;
  s_mic_conflict_active = false;
  s_receiver_pause_requested = false;
  s_low_battery = false;
  s_pause_stationary_enabled = false;
  s_pause_low_power_enabled = false;
  s_silence_mode = AudioCompanionSilenceModeOff;
  s_runlevel = RunLevel_Normal;
  s_error = false;
  s_captured_frames = 0;
  s_sent_frames = 0;
  s_send_backpressure_events = 0;
  s_mic_conflicts = 0;
  s_suppressed_silence_frames = 0;
  s_silence_runs = 0;
  s_silence_last_level = 0;
  s_loss_alerts_posted = 0;
  s_alert_baseline_dropped = 0;
  s_last_alert_uptime_s = 0;
  s_offline_baseline_dropped = 0;
  s_reboot_trace_recorded = false;
  audio_companion_reboot_trace_clear(&s_reboot_trace);
  prv_reset_silence_suppression_locked();
  memset(&s_battery_event_info, 0, sizeof(s_battery_event_info));
}
#endif
