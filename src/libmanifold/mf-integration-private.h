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
MfPermission   mf_integration_kind_get_permission(MfIntegrationKind kind);
guint          mf_integration_get_permissions(MfIntegration *integration);
gboolean       mf_integration_check(MfIntegration *integration,
                                    const char    *component_id,
                                    GError       **error);

MF_INTERNAL_VISIBLE
void mf_integration_diff_privileged(MfIntegration *previous,
                                    MfIntegration *current,
                                    GPtrArray     *added,
                                    GPtrArray     *removed);

MF_END_PRIVATE_DECLS
