/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <systemd/sd-json.h>

#include "mf-integration.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

MfIntegration *mf_integration_new_from_json(sd_json_variant *section,
                                            GError         **error);

MF_END_PRIVATE_DECLS
