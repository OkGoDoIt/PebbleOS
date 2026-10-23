/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// The host's side of installing a .pbw: from a file given on the command
// line, or from the page of a build for the browser. Offers each to the
// firmware on the main loop until it takes it.

#include "samsa_pbw_bottom.h"

#include <stdio.h>
#include <stdlib.h>

#include <posix_host.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define MAX_PENDING 4

static struct {
  void *pbw;
  size_t len;
} s_pending[MAX_PENDING];
static unsigned s_num_pending;

static void prv_offer(void *pbw, size_t len) {
  if (s_num_pending == MAX_PENDING) {
    fprintf(stderr, "Too many .pbw files to install at once\n");
    free(pbw);
    return;
  }
  s_pending[s_num_pending].pbw = pbw;
  s_pending[s_num_pending].len = len;
  s_num_pending++;
}

void samsa_pbw_release(void *pbw) {
  free(pbw);
}

static void prv_poll(void) {
  if (s_num_pending && samsa_pbw_install(s_pending[0].pbw, s_pending[0].len)) {
    s_num_pending--;
    for (unsigned i = 0; i < s_num_pending; i++) {
      s_pending[i] = s_pending[i + 1];
    }
  }
}

POSIX_HOST_HOOK(.poll = prv_poll)

static void prv_install_file(const char *path) {
  FILE *f = fopen(path, "rb");
  void *pbw = nullptr;
  long len = -1;
  if (f && fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
    pbw = malloc(len);
    if (pbw && fread(pbw, 1, len, f) != (size_t)len) {
      free(pbw);
      pbw = nullptr;
    }
  }
  if (f) {
    fclose(f);
  }
  if (!pbw) {
    fprintf(stderr, "Could not read %s\n", path);
    exit(EXIT_FAILURE);
  }
  prv_offer(pbw, len);
}

POSIX_HOST_OPTION(.flag = 'i', .arg = "app.pbw", .help = "install this app",
                  .set = prv_install_file)

#ifdef __EMSCRIPTEN__
// The page puts the .pbw in Module.pbw and asks for it to be installed.
EM_JS(void, prv_copy_from_page, (void *dst), { HEAPU8.set(Module.pbw, dst); });

EMSCRIPTEN_KEEPALIVE void samsa_pbw_install_from_page(size_t len) {
  void *pbw = malloc(len);
  if (!pbw) {
    return;
  }
  prv_copy_from_page(pbw);
  prv_offer(pbw, len);
}
#endif
