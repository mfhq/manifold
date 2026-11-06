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

#include "mf-remote.h"

G_BEGIN_DECLS

#define MF_TYPE_REMOTE_INFO (mf_remote_info_get_type())
G_DECLARE_FINAL_TYPE(MfRemoteInfo, mf_remote_info, MF, REMOTE_INFO, GObject)

MfRemote          *mf_remote_info_get_remote(MfRemoteInfo *info);
const char *const *mf_remote_info_get_available_channels(MfRemoteInfo *info);
const char        *mf_remote_info_get_public_key(MfRemoteInfo *info);
const char        *mf_remote_info_get_fingerprint(MfRemoteInfo *info);

G_END_DECLS
