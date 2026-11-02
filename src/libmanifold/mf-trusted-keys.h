/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(__MANIFOLD_H_INSIDE__) && !defined(MF_COMPILATION)
#error "Only <manifold.h> can be included directly."
#endif

#include <glib-object.h>

#include "mf-signer.h"

G_BEGIN_DECLS

#define MF_TYPE_TRUSTED_KEYS (mf_trusted_keys_get_type())
G_DECLARE_FINAL_TYPE(MfTrustedKeys, mf_trusted_keys, MF, TRUSTED_KEYS, GObject)

MfTrustedKeys *mf_trusted_keys_new(void);

gboolean       mf_trusted_keys_add_key(MfTrustedKeys *keys,
                                       const char    *public_key,
                                       GError       **error);
gboolean       mf_trusted_keys_add_dir(MfTrustedKeys *keys,
                                       const char    *dir,
                                       GError       **error);
gboolean       mf_trusted_keys_is_empty(MfTrustedKeys *keys);

gboolean       mf_trusted_keys_verify(MfTrustedKeys   *keys,
                                      MfSignatureScope scope,
                                      GBytes          *data,
                                      GBytes          *signature,
                                      char           **fingerprint,
                                      GError         **error);

G_END_DECLS
