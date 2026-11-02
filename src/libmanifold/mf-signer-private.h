/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "mf-macros-private.h"
#include "mf-signer.h"

MF_BEGIN_PRIVATE_DECLS

/**
 * MF_SIGNING_KEY_ENV:
 *
 * The environment variable that can hold a private key to sign with.
 */
#define MF_SIGNING_KEY_ENV "MANIFOLD_SIGNING_KEY"

/* the suffix of the public key next to a private key file */
#define MF_PUBLIC_KEY_SUFFIX ".pub"

char       *mf_ssh_key_fingerprint(const char *public_key);
const char *mf_signature_scope_to_namespace(MfSignatureScope scope);

MF_END_PRIVATE_DECLS
