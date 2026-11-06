/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <systemd/sd-json.h>

#include "mf-macros-private.h"
#include "mf-remote-info.h"

MF_BEGIN_PRIVATE_DECLS

MfRemoteInfo *mf_remote_info_new_from_reply(sd_json_variant *reply,
                                            GError         **error);

MF_END_PRIVATE_DECLS
