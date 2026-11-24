/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "mf-error.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

/**
 * MfErrorField:
 * @MF_ERROR_FIELD_COMPONENT_ID: the component that the error is about
 * @MF_ERROR_FIELD_VERSION: the version of the component
 * @MF_ERROR_FIELD_BASE_ID: the base that is missing
 * @MF_ERROR_FIELD_BASE_VERSION: the version of the base that is missing
 * @MF_ERROR_FIELD_IDENTITY: the identity that the missing base must provide
 * @MF_ERROR_FIELD_USED_BY: a component that builds on the component
 * @MF_ERROR_FIELD_ARCHITECTURE: the architecture that the component was built for
 * @MF_ERROR_FIELD_HOST_ARCHITECTURE: the architecture of the host
 * @MF_ERROR_FIELD_KIND: the kind of the installed component with the same ID
 * @MF_ERROR_FIELD_SDK: whether the installed component with the same ID is an SDK, a boolean
 * @MF_ERROR_FIELD_ORIGIN: the origin that was searched for the component
 *
 * A value that an #MfError carries in addition to its message. A field holds
 * a string, or a boolean if its description says so.
 *
 * The install daemon reports errors in English. The fields of an error allow
 * the client to create a translated message that includes the values.
 */
typedef enum {
    MF_ERROR_FIELD_COMPONENT_ID,
    MF_ERROR_FIELD_VERSION,
    MF_ERROR_FIELD_BASE_ID,
    MF_ERROR_FIELD_BASE_VERSION,
    MF_ERROR_FIELD_IDENTITY,
    MF_ERROR_FIELD_USED_BY,
    MF_ERROR_FIELD_ARCHITECTURE,
    MF_ERROR_FIELD_HOST_ARCHITECTURE,
    MF_ERROR_FIELD_KIND,
    MF_ERROR_FIELD_SDK,
    MF_ERROR_FIELD_ORIGIN,
    /*< private >*/
    MF_ERROR_FIELD_LAST
} MfErrorField;

MF_INTERNAL_VISIBLE
void mf_error_set_field(GError     **error,
                        MfErrorField field,
                        const char  *value);
MF_INTERNAL_VISIBLE
const char *mf_error_get_field(const GError *error,
                               MfErrorField  field);
MF_INTERNAL_VISIBLE
void mf_error_set_field_boolean(GError     **error,
                                MfErrorField field,
                                gboolean     value);
MF_INTERNAL_VISIBLE
gboolean mf_error_get_field_boolean(const GError *error,
                                    MfErrorField  field);

MF_END_PRIVATE_DECLS
