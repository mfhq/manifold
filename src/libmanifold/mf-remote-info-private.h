/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <systemd/sd-json.h>

#include "mf-macros-private.h"
#include "mf-remote-info.h"
#include "mf-repo-private.h"

MF_BEGIN_PRIVATE_DECLS

MF_INTERNAL_VISIBLE
MfRemoteInfo *mf_remote_info_new_for_root_index(MfRepoRootIndex *index,
                                                const char      *url,
                                                const char      *public_key);
MfRemoteInfo *mf_remote_info_new_from_reply(sd_json_variant *reply,
                                            GError         **error);
MF_INTERNAL_VISIBLE
int mf_remote_info_to_reply(MfRemoteInfo     *info,
                            sd_json_variant **ret_reply);

MF_END_PRIVATE_DECLS
