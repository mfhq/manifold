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

#include "mf-trusted-keys.h"

G_BEGIN_DECLS

#define MF_TYPE_REMOTE (mf_remote_get_type())
G_DECLARE_FINAL_TYPE(MfRemote, mf_remote, MF, REMOTE, GObject)

MfRemote          *mf_remote_new_for_id(const char *remote_id,
                                        GError    **error);
GPtrArray         *mf_remote_list_all(GError **error);

const char        *mf_remote_get_id(MfRemote *remote);
const char        *mf_remote_get_url(MfRemote *remote);
const char *const *mf_remote_get_channels(MfRemote *remote);

MfTrustedKeys     *mf_remote_load_trusted_keys(MfRemote *remote,
                                               GError  **error);

G_END_DECLS
