/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/audio_companion_private.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! Durable receiver registry for the audio companion service. Stores at most
//! one authorized receiver as SHA-256(receiver_id) + display name + bind time
//! in a dedicated settings file. The raw receiver id is never persisted.
//! Fail-closed: unknown or mismatched identities never match.

typedef enum {
  //! No receiver is bound; user consent is required to bind this one.
  AudioCompanionAuthEvalNoReceiver = 0,
  //! The presented id hashes to the stored identity.
  AudioCompanionAuthEvalMatch,
  //! A different receiver is bound; deny and require revoke-on-watch first.
  AudioCompanionAuthEvalMismatch,
} AudioCompanionAuthEval;

void audio_companion_auth_init(void);

bool audio_companion_auth_receiver_exists(void);

//! Copies the stored receiver display name (NUL-terminated). Returns false if
//! no receiver is bound.
bool audio_companion_auth_get_receiver_name(char *buf, size_t buf_size);

AudioCompanionAuthEval audio_companion_auth_evaluate(
    const uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES]);

//! Bind a receiver. Refuses (returns false) if a receiver is already bound;
//! revoke first. Call only after explicit user consent on the watch.
bool audio_companion_auth_store_receiver(
    const uint8_t receiver_id[AUDIO_COMPANION_RECEIVER_ID_BYTES], const char *name);

//! Wipe the stored identity (user revoke on watch or AUTH_REVOKE from the app).
void audio_companion_auth_forget_receiver(void);
