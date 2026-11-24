/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include <manifold.h>

#include "mf-manifest-private.h"

G_BEGIN_DECLS

/**
 * MfdErrorDetail:
 * @component_id: the component that the error is about
 * @version: the version of @component_id, for %MF_ERROR_ALREADY_INSTALLED
 * @base_id: the missing base, for %MF_ERROR_MISSING_BASE
 * @base_version: the version of the missing base, for %MF_ERROR_MISSING_BASE
 * @identity: the identity that the missing base must provide, for %MF_ERROR_MISSING_BASE
 * @used_by: a component that needs @component_id, for %MF_ERROR_COMPONENT_IN_USE
 * @architecture: the architecture that @component_id was built for, for %MF_ERROR_WRONG_ARCHITECTURE
 * @kind: the kind of the installed component with the ID @component_id, for %MF_ERROR_ID_CONFLICT
 * @sdk: whether the installed component is an SDK, for %MF_ERROR_ID_CONFLICT
 *
 * Details about the failure of an operation, beyond the #GError.
 */
typedef struct {
    char           *component_id;
    char           *version;
    char           *base_id;
    char           *base_version;
    char           *identity;
    char           *used_by;
    char           *architecture;
    MfComponentKind kind;
    gboolean        sdk;
} MfdErrorDetail;

void mfd_error_detail_clear(MfdErrorDetail *detail);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfdErrorDetail,
                                 mfd_error_detail_clear)

void mfd_set_error_already_installed(GError        **error,
                                     MfdErrorDetail *ret_detail,
                                     const char     *component_id,
                                     const char     *version);
void mfd_set_error_not_installed(GError        **error,
                                 MfdErrorDetail *ret_detail,
                                 const char     *component_id,
                                 const char     *version);
void mfd_set_error_in_use(GError        **error,
                          MfdErrorDetail *ret_detail,
                          const char     *component_id,
                          const char     *used_by);
void mfd_set_error_missing_base(GError               **error,
                                MfdErrorDetail        *ret_detail,
                                const char            *component_id,
                                const MfBaseReference *ref);
void mfd_set_error_id_conflict(GError        **error,
                               MfdErrorDetail *ret_detail,
                               MfComponent    *installed);
void mfd_set_error_wrong_architecture(GError        **error,
                                      MfdErrorDetail *ret_detail,
                                      const char     *component_id,
                                      const char     *architecture);

G_END_DECLS
