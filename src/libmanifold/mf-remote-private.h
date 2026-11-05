/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "mf-remote.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

/* the directory with the remotes in MF_DATADIR and MF_SYSCONFDIR, and the files of a remote */
#define MF_REMOTES_SUBDIR     "remotes"
#define MF_REMOTE_CONFIG_NAME "remote.conf"
#define MF_REMOTE_KEYS_SUBDIR "keys"

MF_INTERNAL_VISIBLE
MfRemote *mf_remote_new_for_dirs(const char *vendor_dir,
                                 const char *admin_dir,
                                 const char *remote_id,
                                 GError    **error);
MF_INTERNAL_VISIBLE
GPtrArray *mf_remote_list_for_dirs(const char *vendor_dir,
                                   const char *admin_dir,
                                   GError    **error);

MF_END_PRIVATE_DECLS
