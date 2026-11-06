/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <systemd/sd-json.h>

#include "mf-remote.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

/* the directory with the remotes in MF_DATADIR and MF_SYSCONFDIR, and the files of a remote */
#define MF_REMOTES_SUBDIR     "remotes"
#define MF_REMOTE_CONFIG_NAME "remote.conf"
#define MF_REMOTE_KEYS_SUBDIR "keys"

/* the directory for remotes that vendors ship, and the one for remotes of the administrator */
#define MF_REMOTES_VENDOR_DIR MF_DATADIR "/" MF_REMOTES_SUBDIR
#define MF_REMOTES_ADMIN_DIR  MF_SYSCONFDIR "/" MF_REMOTES_SUBDIR

/* the channel of a remote that does not configure any channels */
#define MF_REMOTE_DEFAULT_CHANNEL "main"

MF_INTERNAL_VISIBLE
gboolean mf_remote_id_is_valid(const char *remote_id);
MF_INTERNAL_VISIBLE
gboolean mf_remote_channel_is_valid(const char *channel);
MF_INTERNAL_VISIBLE
char *mf_origin_build_name(const char *remote_id,
                           const char *channel);
MF_INTERNAL_VISIBLE
gboolean mf_origin_split_name(const char  *origin,
                              char       **ret_remote_id,
                              const char **ret_channel);
MF_INTERNAL_VISIBLE
gboolean mf_remote_check_channels(const char        *remote_id,
                                  const char *const *channels,
                                  GError           **error);
MF_INTERNAL_VISIBLE
char *mf_remote_normalize_url(const char *url);

MF_INTERNAL_VISIBLE
MfRemote *mf_remote_new_for_dirs(const char *vendor_dir,
                                 const char *admin_dir,
                                 const char *state_dir,
                                 const char *remote_id,
                                 GError    **error);
MF_INTERNAL_VISIBLE
GPtrArray *mf_remote_list_for_dirs(const char *vendor_dir,
                                   const char *admin_dir,
                                   const char *state_dir,
                                   GError    **error);

MfRemote  *mf_remote_new_from_json(sd_json_variant *object);

MF_INTERNAL_VISIBLE
char *mf_remote_build_url(MfRemote   *remote,
                          const char *location);

MF_END_PRIVATE_DECLS
