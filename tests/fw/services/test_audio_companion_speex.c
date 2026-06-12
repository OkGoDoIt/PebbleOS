/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "pbl/services/audio_companion_private.h"

#include "speex/speex.h"
#include "speex/speex_bits.h"

#include "stubs_pbl_malloc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Real-Speex golden fixture test (implementation plan Section 7 / Decision F).
//
// The audio companion stream carries frames produced by the firmware's vendored fixed-point
// Speex encoder with the exact settings of src/fw/services/voice/voice_speex.c. This test
// compiles that vendored encoder/decoder for the host, encodes a deterministic synthetic
// voice-like signal, and locks the resulting bitstream with a golden hash: FIXED_POINT Speex
// is pure integer math, so the encoded bytes are identical on every platform. If this test
// ever fails, the wire bitstream changed and shipping receivers would stop decoding —
// treat it as a protocol-version decision, not a test to update casually.
//
// The same bytes are exported as fixtures for the app repo
// (pebble-audio-companion/spec/fixtures/speex_frames_v1*) by running this test once with
// AUDIO_COMPANION_SPEEX_FIXTURE_DIR=<dir> in the environment.

// Mirror of the voice_speex.c encoder configuration. Keep in sync.
#define SPEEX_TEST_SAMPLE_RATE 16000
#define SPEEX_TEST_BIT_RATE 9800
#define SPEEX_TEST_QUALITY 6
#define SPEEX_TEST_COMPLEXITY 1
#define SPEEX_TEST_AUDIO_GAIN 3

#define FRAME_SAMPLES AUDIO_COMPANION_DEFAULT_FRAME_SAMPLES // 320 = 20 ms @ 16 kHz
#define TEST_FRAMES 50                                      // 1 second of audio
#define TOTAL_SAMPLES (FRAME_SAMPLES * TEST_FRAMES)

extern const SpeexMode speex_wb_mode;

// Golden values captured from the first run of this test on the vendored encoder
// (third_party/speex @ FIXED_POINT, wb mode, quality 6, complexity 1, 9800 bps).
#define GOLDEN_FRAME_BYTES 25
#define GOLDEN_STREAM_LEN 1350 // 50 frames x (2-byte length prefix + 25 bytes)
#define GOLDEN_STREAM_FNV1A 0x490aea30u
static const uint8_t GOLDEN_FRAME0_PREFIX[8] = {0x1f, 0x70, 0x9d, 0x80, 0x00, 0x39, 0xce, 0x70};

// --- deterministic input -----------------------------------------------------------------------

// Integer triangle wave: period `period` samples, peak amplitude `amp`.
static int32_t prv_tri(uint32_t n, uint32_t period, int32_t amp) {
  uint32_t phase = n % period;
  uint32_t half = period / 2;
  if (phase < half) {
    return -amp + (int32_t)((2 * (int64_t)amp * phase) / half);
  }
  return amp - (int32_t)((2 * (int64_t)amp * (phase - half)) / half);
}

// Voice-like deterministic signal: a 160 Hz fundamental (period 100 samples at 16 kHz) with
// two harmonics and a coarse 80 ms on/off-ish amplitude envelope. Integer math only, so the
// signal — and therefore the encoded bitstream — is identical everywhere.
static int16_t prv_input_sample(uint32_t n) {
  int32_t value = prv_tri(n, 100, 2400) + prv_tri(n, 50, 1200) + prv_tri(n, 25, 600);
  static const int32_t envelope_pct[4] = {100, 70, 85, 55};
  value = (value * envelope_pct[(n / 1280) % 4]) / 100;
  return (int16_t)value;
}

// Gain step copied from voice_speex_encode_frame(): the wire carries gained audio.
static int16_t prv_apply_gain(int16_t sample) {
  int32_t boosted = (int32_t)sample * SPEEX_TEST_AUDIO_GAIN;
  if (boosted > INT16_MAX) {
    boosted = INT16_MAX;
  } else if (boosted < INT16_MIN) {
    boosted = INT16_MIN;
  }
  return (int16_t)boosted;
}

static uint32_t prv_fnv1a(const uint8_t *data, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash = (hash ^ data[i]) * 16777619u;
  }
  return hash;
}

// --- shared encode helper ----------------------------------------------------------------------

typedef struct {
  uint8_t stream[TEST_FRAMES * (2 + AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES)];
  size_t stream_len;
  uint16_t frame_len[TEST_FRAMES];
  int16_t gained[TOTAL_SAMPLES];
} EncodedStream;

static void prv_encode_all(EncodedStream *out) {
  memset(out, 0, sizeof(*out));

  void *enc = speex_encoder_init(&speex_wb_mode);
  cl_assert(enc != NULL);

  int tmp = SPEEX_TEST_QUALITY;
  speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &tmp);
  tmp = SPEEX_TEST_COMPLEXITY;
  speex_encoder_ctl(enc, SPEEX_SET_COMPLEXITY, &tmp);
  tmp = SPEEX_TEST_SAMPLE_RATE;
  speex_encoder_ctl(enc, SPEEX_SET_SAMPLING_RATE, &tmp);
  tmp = SPEEX_TEST_BIT_RATE;
  speex_encoder_ctl(enc, SPEEX_SET_BITRATE, &tmp);

  int frame_size = 0;
  speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
  cl_assert_equal_i(frame_size, FRAME_SAMPLES);

  for (uint32_t n = 0; n < TOTAL_SAMPLES; n++) {
    out->gained[n] = prv_apply_gain(prv_input_sample(n));
  }

  SpeexBits bits;
  speex_bits_init(&bits);
  for (int frame = 0; frame < TEST_FRAMES; frame++) {
    int16_t samples[FRAME_SAMPLES];
    memcpy(samples, &out->gained[frame * FRAME_SAMPLES], sizeof(samples));

    speex_bits_reset(&bits);
    speex_encode_int(enc, samples, &bits);

    uint8_t encoded[AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES];
    int encoded_bytes = speex_bits_write(&bits, (char *)encoded, sizeof(encoded));
    cl_assert(encoded_bytes > 0);
    cl_assert(encoded_bytes <= AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES);

    out->frame_len[frame] = (uint16_t)encoded_bytes;
    out->stream[out->stream_len++] = (uint8_t)(encoded_bytes & 0xff);
    out->stream[out->stream_len++] = (uint8_t)((encoded_bytes >> 8) & 0xff);
    memcpy(&out->stream[out->stream_len], encoded, (size_t)encoded_bytes);
    out->stream_len += (size_t)encoded_bytes;
  }
  speex_bits_destroy(&bits);
  speex_encoder_destroy(enc);
}

// --- tests --------------------------------------------------------------------------------------

void test_audio_companion_speex__bitstream_is_golden(void) {
  EncodedStream *enc = malloc(sizeof(*enc));
  cl_assert(enc != NULL);
  prv_encode_all(enc);

  // CBR: every frame has the same size.
  for (int frame = 0; frame < TEST_FRAMES; frame++) {
    cl_assert_equal_i(enc->frame_len[frame], enc->frame_len[0]);
  }

  const uint32_t hash = prv_fnv1a(enc->stream, enc->stream_len);
  if (enc->frame_len[0] != GOLDEN_FRAME_BYTES || hash != GOLDEN_STREAM_FNV1A) {
    printf("speex golden mismatch: frame_bytes=%u stream_len=%zu fnv1a=0x%08x "
           "frame0=%02x %02x %02x %02x %02x %02x %02x %02x\n",
           enc->frame_len[0], enc->stream_len, hash,
           enc->stream[2], enc->stream[3], enc->stream[4], enc->stream[5],
           enc->stream[6], enc->stream[7], enc->stream[8], enc->stream[9]);
  }
  cl_assert_equal_i(enc->frame_len[0], GOLDEN_FRAME_BYTES);
  cl_assert_equal_i((int)enc->stream_len, GOLDEN_STREAM_LEN);
  cl_assert(hash == GOLDEN_STREAM_FNV1A);
  // First bytes of frame 0 (after the u16 length prefix) spelled out for quick triage.
  cl_assert_equal_i(memcmp(&enc->stream[2], GOLDEN_FRAME0_PREFIX,
                           sizeof(GOLDEN_FRAME0_PREFIX)), 0);

  // Optional fixture export for the app repo (spec/fixtures/speex_frames_v1*):
  // AUDIO_COMPANION_SPEEX_FIXTURE_DIR=<dir> ./waf test -M test_audio_companion_speex
  const char *fixture_dir = getenv("AUDIO_COMPANION_SPEEX_FIXTURE_DIR");
  if (fixture_dir != NULL) {
    char path[512];
    snprintf(path, sizeof(path), "%s/speex_frames_v1.bin", fixture_dir);
    FILE *f = fopen(path, "wb");
    cl_assert(f != NULL);
    cl_assert_equal_i(fwrite(enc->stream, 1, enc->stream_len, f), (int)enc->stream_len);
    fclose(f);

    snprintf(path, sizeof(path), "%s/speex_frames_v1_input.pcm", fixture_dir);
    f = fopen(path, "wb");
    cl_assert(f != NULL);
    cl_assert_equal_i(fwrite(enc->gained, sizeof(int16_t), TOTAL_SAMPLES, f), TOTAL_SAMPLES);
    fclose(f);
  }

  free(enc);
}

void test_audio_companion_speex__decoder_recovers_signal(void) {
  EncodedStream *enc = malloc(sizeof(*enc));
  cl_assert(enc != NULL);
  prv_encode_all(enc);

  void *dec = speex_decoder_init(&speex_wb_mode);
  cl_assert(dec != NULL);
  int tmp = 1;
  speex_decoder_ctl(dec, SPEEX_SET_ENH, &tmp);

  static int16_t decoded[TOTAL_SAMPLES];
  SpeexBits bits;
  speex_bits_init(&bits);
  size_t offset = 0;
  for (int frame = 0; frame < TEST_FRAMES; frame++) {
    const uint16_t len = (uint16_t)(enc->stream[offset] | (enc->stream[offset + 1] << 8));
    offset += 2;
    speex_bits_read_from(&bits, (char *)&enc->stream[offset], len);
    offset += len;
    const int ret = speex_decode_int(dec, &bits, &decoded[frame * FRAME_SAMPLES]);
    cl_assert_equal_i(ret, 0);
  }
  speex_bits_destroy(&bits);
  speex_decoder_destroy(dec);

  // Skip the first two frames (codec warm-up), then check the decoder reproduced the input:
  // normalized cross-correlation peak over small positive lags (codec lookahead delay).
  const int start = 2 * FRAME_SAMPLES;
  const int count = TOTAL_SAMPLES - start - 512;
  double best_corr = -1.0;
  for (int lag = 0; lag <= 480; lag++) {
    double dot = 0, in_sq = 0, out_sq = 0;
    for (int i = 0; i < count; i++) {
      const double in = enc->gained[start + i];
      const double out = decoded[start + i + lag];
      dot += in * out;
      in_sq += in * in;
      out_sq += out * out;
    }
    if (in_sq > 0 && out_sq > 0) {
      const double corr = dot / (sqrt(in_sq) * sqrt(out_sq));
      if (corr > best_corr) {
        best_corr = corr;
      }
    }
  }
  if (best_corr <= 0.5) {
    printf("speex decode correlation too low: %f\n", best_corr);
  }
  cl_assert(best_corr > 0.5);

  // Energy sanity: decoded loudness within 4x either way of the input loudness.
  double in_energy = 0, out_energy = 0;
  for (int i = start; i < TOTAL_SAMPLES; i++) {
    in_energy += (double)enc->gained[i] * enc->gained[i];
    out_energy += (double)decoded[i] * decoded[i];
  }
  cl_assert(out_energy > in_energy / 16.0);
  cl_assert(out_energy < in_energy * 16.0);

  free(enc);
}

void test_audio_companion_speex__silence_encodes_within_bounds(void) {
  void *enc = speex_encoder_init(&speex_wb_mode);
  cl_assert(enc != NULL);
  int tmp = SPEEX_TEST_QUALITY;
  speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &tmp);
  tmp = SPEEX_TEST_COMPLEXITY;
  speex_encoder_ctl(enc, SPEEX_SET_COMPLEXITY, &tmp);
  tmp = SPEEX_TEST_SAMPLE_RATE;
  speex_encoder_ctl(enc, SPEEX_SET_SAMPLING_RATE, &tmp);
  tmp = SPEEX_TEST_BIT_RATE;
  speex_encoder_ctl(enc, SPEEX_SET_BITRATE, &tmp);

  SpeexBits bits;
  speex_bits_init(&bits);
  for (int frame = 0; frame < 5; frame++) {
    int16_t samples[FRAME_SAMPLES] = {0};
    speex_bits_reset(&bits);
    speex_encode_int(enc, samples, &bits);
    uint8_t encoded[AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES];
    const int encoded_bytes = speex_bits_write(&bits, (char *)encoded, sizeof(encoded));
    cl_assert(encoded_bytes > 0);
    cl_assert(encoded_bytes <= AUDIO_COMPANION_MAX_ENCODED_FRAME_BYTES);
  }
  speex_bits_destroy(&bits);
  speex_encoder_destroy(enc);
}
