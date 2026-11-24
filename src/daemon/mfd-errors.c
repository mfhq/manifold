/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-errors.h"

#include "mf-utils-private.h"

/*
 * Errors of the install daemon that carry fields.
 *
 * The daemon describes an error in English. The client translates errors, and
 * it can only include a value in a translated message if the error has the
 * value as a field, see #MfErrorField. Each function here sets an error
 * together with the fields that the client needs for its message.
 */

/**
 * mfd_set_error_already_installed:
 */
void
mfd_set_error_already_installed(GError **error, const char *component_id, const char *version)
{
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_ALREADY_INSTALLED,
                "%s %s is installed already",
                component_id,
                version);
    mf_error_set_field(error, MF_ERROR_FIELD_COMPONENT_ID, component_id);
    mf_error_set_field(error, MF_ERROR_FIELD_VERSION, version);
}

/**
 * mfd_set_error_not_installed:
 * @version: (nullable): the version, or %NULL for any version of the component
 */
void
mfd_set_error_not_installed(GError **error, const char *component_id, const char *version)
{
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_NOT_FOUND,
                "%s%s%s is not installed",
                component_id,
                version != NULL ? " " : "",
                version != NULL ? version : "");
    mf_error_set_field(error, MF_ERROR_FIELD_COMPONENT_ID, component_id);
    mf_error_set_field(error, MF_ERROR_FIELD_VERSION, version);
}

/**
 * mfd_set_error_in_use:
 * @used_by: the ID of a component that is kept, and that builds on @component_id
 */
void
mfd_set_error_in_use(GError **error, const char *component_id, const char *used_by)
{
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_COMPONENT_IN_USE,
                "Component %s is used by %s",
                component_id,
                used_by);
    mf_error_set_field(error, MF_ERROR_FIELD_COMPONENT_ID, component_id);
    mf_error_set_field(error, MF_ERROR_FIELD_USED_BY, used_by);
}

/**
 * mfd_set_error_not_available:
 * @component_id: the component that was not found in the remotes
 * @origin: (nullable): the only origin that was searched, or %NULL if all remotes were searched
 */
void
mfd_set_error_not_available(GError **error, const char *component_id, const char *origin)
{
    if (origin != NULL)
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_AVAILABLE, "%s has no component %s", origin, component_id);
    else
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_AVAILABLE, "No remote has %s", component_id);
    mf_error_set_field(error, MF_ERROR_FIELD_COMPONENT_ID, component_id);
    mf_error_set_field(error, MF_ERROR_FIELD_ORIGIN, origin);
}

/**
 * mfd_base_reference_build_version:
 *
 * Returns: (transfer full): the version that an error reports for a
 *   reference, with the revision if the exact revision is required
 */
static char *
mfd_base_reference_build_version(const MfBaseReference *ref)
{
    return ref->exact ? mf_build_full_version(ref->version, ref->revision) : g_strdup(ref->version);
}

/**
 * mfd_set_error_base_not_available:
 * @component_id: the component whose base or base SDK is missing
 * @ref: the reference to the component that is not installed, and that no remote has
 */
void
mfd_set_error_base_not_available(GError **error, const char *component_id, const MfBaseReference *ref)
{
    g_autofree char *version = mfd_base_reference_build_version(ref);

    g_set_error(error,
                MF_ERROR,
                MF_ERROR_BASE_NOT_AVAILABLE,
                "%s needs base %s %s (identity %s), which is not available in any remote",
                component_id,
                ref->id,
                version,
                ref->identity);
    mf_error_set_field(error, MF_ERROR_FIELD_COMPONENT_ID, component_id);
    mf_error_set_field(error, MF_ERROR_FIELD_BASE_ID, ref->id);
    mf_error_set_field(error, MF_ERROR_FIELD_BASE_VERSION, version);
    mf_error_set_field(error, MF_ERROR_FIELD_IDENTITY, ref->identity);
}

/**
 * mfd_set_error_missing_base:
 * @component_id: the component whose base or base SDK is missing
 * @ref: the reference to the missing component
 */
void
mfd_set_error_missing_base(GError **error, const char *component_id, const MfBaseReference *ref)
{
    g_autofree char *version = mfd_base_reference_build_version(ref);

    g_set_error(error,
                MF_ERROR,
                MF_ERROR_MISSING_BASE,
                "%s needs base %s %s (identity %s), which is not installed",
                component_id,
                ref->id,
                version,
                ref->identity);
    mf_error_set_field(error, MF_ERROR_FIELD_COMPONENT_ID, component_id);
    mf_error_set_field(error, MF_ERROR_FIELD_BASE_ID, ref->id);
    mf_error_set_field(error, MF_ERROR_FIELD_BASE_VERSION, version);
    mf_error_set_field(error, MF_ERROR_FIELD_IDENTITY, ref->identity);
}

/**
 * mfd_set_error_id_conflict:
 * @installed: the installed version that has the same ID, but is of a different kind
 */
void
mfd_set_error_id_conflict(GError **error, MfComponent *installed)
{
    const char *component_id = mf_component_get_id(installed);
    const char *kind = mf_component_kind_to_string(mf_component_get_kind(installed));
    gboolean sdk = mf_component_is_sdk(installed);

    g_set_error(error,
                MF_ERROR,
                MF_ERROR_ID_CONFLICT,
                "%s is installed already as another kind of component (%s%s)",
                component_id,
                kind,
                sdk ? " SDK" : "");
    mf_error_set_field(error, MF_ERROR_FIELD_COMPONENT_ID, component_id);
    mf_error_set_field(error, MF_ERROR_FIELD_KIND, kind);
    mf_error_set_field_boolean(error, MF_ERROR_FIELD_SDK, sdk);
}

/**
 * mfd_set_error_wrong_architecture:
 */
void
mfd_set_error_wrong_architecture(GError **error, const char *component_id, const char *architecture)
{
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_WRONG_ARCHITECTURE,
                "%s is built for %s, but this system is %s",
                component_id,
                architecture,
                mf_get_host_architecture());
    mf_error_set_field(error, MF_ERROR_FIELD_COMPONENT_ID, component_id);
    mf_error_set_field(error, MF_ERROR_FIELD_ARCHITECTURE, architecture);
    mf_error_set_field(error, MF_ERROR_FIELD_HOST_ARCHITECTURE, mf_get_host_architecture());
}
