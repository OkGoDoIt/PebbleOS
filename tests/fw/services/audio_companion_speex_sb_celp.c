/* SPDX-License-Identifier: Apache-2.0 */

// Vendored Speex for test_audio_companion_speex, built for the host with the watch's defines.
// The host test build is clang -Wall -Werror, and in the fixed-point, no-VBR configuration
// sb_celp.c carries dead locals; silence just those rather than patching the submodule.
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "../../../third_party/speex/speex/libspeex/sb_celp.c"
