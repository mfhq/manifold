/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-errors.h"

#include "mf-utils-private.h"

/**
 * mfd_error_detail_clear:
 */
void
mfd_error_detail_clear(MfdErrorDetail *detail)
{
    g_clear_pointer(&detail->component_id, g_free);
    g_clear_pointer(&detail->version, g_free);
    g_clear_pointer(&detail->base_id, g_free);
    g_clear_pointer(&detail->base_version, g_free);
    g_clear_pointer(&detail->identity, g_free);
    g_clear_pointer(&detail->used_by, g_free);
    g_clear_pointer(&detail->architecture, g_free);
    detail->kind = MF_COMPONENT_KIND_UNKNOWN;
    detail->sdk = FALSE;
}

/**
 * mfd_set_error_already_installed:
 */
void
mfd_set_error_already_installed(GError **error,
                                MfdErrorDetail *ret_detail,
                                const char *component_id,
                                const char *version)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->version = g_strdup(version);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_ALREADY_INSTALLED,
                "%s %s is installed already",
                component_id,
                version);
}

/**
 * mfd_set_error_not_installed:
 * @version: (nullable): the version, or %NULL for any version of the component
 */
void
mfd_set_error_not_installed(GError **error,
                            MfdErrorDetail *ret_detail,
                            const char *component_id,
                            const char *version)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->version = g_strdup(version);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_NOT_FOUND,
                "%s%s%s is not installed",
                component_id,
                version != NULL ? " " : "",
                version != NULL ? version : "");
}

/**
 * mfd_set_error_in_use:
 * @used_by: the ID of a component that is kept, and that builds on @component_id
 */
void
mfd_set_error_in_use(GError **error,
                     MfdErrorDetail *ret_detail,
                     const char *component_id,
                     const char *used_by)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->used_by = g_strdup(used_by);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_COMPONENT_IN_USE,
                "Component %s is used by %s",
                component_id,
                used_by);
}

/**
 * mfd_set_error_missing_base:
 * @component_id: the component whose base or base SDK is missing
 * @ref: the reference to the missing component
 */
void
mfd_set_error_missing_base(GError **error,
                           MfdErrorDetail *ret_detail,
                           const char *component_id,
                           const MfBaseReference *ref)
{
    g_autofree char *version = NULL;

    /* the revision is only reported if the exact revision is required */
    version = ref->exact ? mf_build_full_version(ref->version, ref->revision) : g_strdup(ref->version);
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->base_id = g_strdup(ref->id);
        ret_detail->base_version = g_strdup(version);
        ret_detail->identity = g_strdup(ref->identity);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_MISSING_BASE,
                "%s needs base %s %s (identity %s), which is not installed",
                component_id,
                ref->id,
                version,
                ref->identity);
}

/**
 * mfd_set_error_id_conflict:
 * @installed: the installed version that has the same ID, but is of a different kind
 */
void
mfd_set_error_id_conflict(GError **error, MfdErrorDetail *ret_detail, MfComponent *installed)
{
    const char *component_id = mf_component_get_id(installed);
    MfComponentKind kind = mf_component_get_kind(installed);
    gboolean sdk = mf_component_is_sdk(installed);

    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->kind = kind;
        ret_detail->sdk = sdk;
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_ID_CONFLICT,
                "%s is installed already as another kind of component (%s%s)",
                component_id,
                mf_component_kind_to_string(kind),
                sdk ? " SDK" : "");
}

/**
 * mfd_set_error_wrong_architecture:
 */
void
mfd_set_error_wrong_architecture(GError **error,
                                 MfdErrorDetail *ret_detail,
                                 const char *component_id,
                                 const char *architecture)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->architecture = g_strdup(architecture);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_WRONG_ARCHITECTURE,
                "%s is built for %s, but this system is %s",
                component_id,
                architecture,
                mf_get_host_architecture());
}
