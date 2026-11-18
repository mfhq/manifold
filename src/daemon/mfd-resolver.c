/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-resolver.h"

#include <string.h>
#include <glib/gi18n-lib.h>

#include "mf-component-private.h"
#include "mf-manifest-private.h"
#include "mf-remote-private.h"
#include "mf-repo-private.h"
#include "mf-store-private.h"
#include "mfd-repo-image.h"

/*
 * Find the components to install from the remotes. We look up the requested
 * components in the origins, together with the bases they need that are not
 * installed yet. We download and verify their manifests, and add them to an
 * install set. The images are only downloaded when the set is installed.
 */

/* the highest number of bases that we resolve for one component, each one building on the next */
constexpr guint BASE_DEPTH_MAX = 8;

/**
 * mfd_repo_request_new_install:
 * @component_id: the component to install
 * @origin: (nullable): the origin to install it from, or %NULL if only one origin has it
 *
 * Returns: (transfer full): a request to install a component from the remotes
 */
MfdRepoRequest *
mfd_repo_request_new_install(const char *component_id, const char *origin)
{
    MfdRepoRequest *request = g_new0(MfdRepoRequest, 1);

    request->component_id = g_strdup(component_id);
    request->origin = g_strdup(origin);
    return request;
}

/**
 * mfd_repo_request_free:
 */
void
mfd_repo_request_free(MfdRepoRequest *request)
{
    if (request == NULL)
        return;
    g_free(request->component_id);
    g_free(request->origin);
    g_free(request);
}

/* the state of resolving one request */
typedef struct {
    MfdInstallSet *set;
    MfStore *store;
    MfdFetcher *fetcher;
    MfdOriginSet *origin_set;
} MfdResolver;

/**
 * mfd_resolver_fetch_manifest:
 *
 * Download the manifest of a version, and verify it.
 *
 * Returns: (transfer full): the manifest, or %NULL
 */
static MfManifest *
mfd_resolver_fetch_manifest(MfdResolver *resolver,
                            MfdOrigin *origin,
                            const char *component_id,
                            MfRepoVersion *version,
                            GError **error)
{
    g_autofree char *url = mf_remote_build_url(origin->remote, version->manifest_location);
    g_autoptr(GBytes) data = NULL;

    mfd_install_set_add_remote(resolver->set, origin->remote);
    data = mfd_fetcher_download_file(resolver->fetcher, url, version->manifest.size, error);
    if (data == NULL)
        return NULL;
    return mf_repo_version_read_manifest(version, data, component_id, mf_get_host_architecture(), error);
}

/**
 * mfd_resolver_add:
 * @dependency: whether the version is only installed because another component needs it
 *
 * Add a version of a component to the set.
 *
 * Returns: (transfer none): the new item
 */
static MfdInstallItem *
mfd_resolver_add(MfdResolver *resolver,
                 MfdOrigin *origin,
                 MfRepoVersion *version,
                 MfManifest *manifest,
                 gboolean dependency)
{
    MfdInstallItem *item = mfd_install_set_add(resolver->set, manifest, version->bundle, origin->name);

    item->is_dependency = dependency;
    mfd_repo_image_set_source(item, resolver->store, resolver->fetcher, origin, version);
    return item;
}

/**
 * mfd_resolver_has_base:
 *
 * Returns: %TRUE if the base of @manifest is installed, or is in the set
 */
static gboolean
mfd_resolver_has_base(MfdResolver *resolver, MfManifest *manifest)
{
    const char *base_id = mf_manifest_get_base_id(manifest);
    const char *identity = mf_manifest_get_base_identity(manifest);
    g_autoptr(MfComponent) provider = NULL;
    g_autoptr(GPtrArray) versions = NULL;

    if (!mf_manifest_needs_exact_base(manifest)) {
        if (mfd_install_set_find_identity(resolver->set, identity) != NULL)
            return TRUE;
        provider = mf_store_find_identity(resolver->store, identity, NULL);
        return provider != NULL;
    }

    if (mfd_install_set_find_exact_base(resolver->set, manifest) != NULL)
        return TRUE;
    versions = mf_store_list_components(resolver->store, base_id, MF_COMPONENT_KIND_UNKNOWN, NULL);
    for (guint i = 0; versions != NULL && i < versions->len; i++) {
        MfComponent *component = g_ptr_array_index(versions, i);

        if (g_str_equal(mf_component_get_version(component), mf_manifest_get_base_version(manifest)) &&
            mf_component_get_revision(component) == mf_manifest_get_base_revision(manifest) &&
            g_strcmp0(mf_component_get_identity(component), identity) == 0)
            return TRUE;
    }
    return FALSE;
}

/**
 * mfd_resolver_find_base:
 * @origin: the origin of the component, which is searched first
 * @ret_item: (out) (transfer none): the item of the base, or %NULL if no origin has it
 *
 * Find the base of a component in the origins, and add it to the set.
 *
 * A component that needs its exact base only accepts the version and revision
 * its manifest names. Any other component accepts every version that provides
 * the identity its manifest names, and we try the named version first. The
 * identity of a version is only recorded in its manifest, so we download
 * manifests until we find a matching one.
 *
 * Returns: %FALSE if a manifest could not be downloaded or verified
 */
static gboolean
mfd_resolver_find_base(MfdResolver *resolver,
                       MfdOrigin *origin,
                       MfManifest *manifest,
                       MfdInstallItem **ret_item,
                       GError **error)
{
    const char *base_id = mf_manifest_get_base_id(manifest);
    const char *base_version = mf_manifest_get_base_version(manifest);
    guint64 base_revision = mf_manifest_get_base_revision(manifest);
    gboolean exact = mf_manifest_needs_exact_base(manifest);
    GPtrArray *origins = resolver->origin_set->origins;
    g_autoptr(GPtrArray) order = g_ptr_array_new();

    *ret_item = NULL;
    g_ptr_array_add(order, origin);
    for (guint i = 0; i < origins->len; i++) {
        if (g_ptr_array_index(origins, i) != origin)
            g_ptr_array_add(order, g_ptr_array_index(origins, i));
    }

    for (guint i = 0; i < order->len; i++) {
        MfdOrigin *candidate_origin = g_ptr_array_index(order, i);
        GPtrArray *versions = mf_repo_component_index_get_versions(candidate_origin->components, base_id);

        /* the first pass tries the named version, the second pass all other versions, newest first */
        for (guint pass = 0; versions != NULL && pass < (exact ? 1 : 2); pass++) {
            for (guint j = 0; j < versions->len; j++) {
                MfRepoVersion *version = g_ptr_array_index(versions, j);
                gboolean named = g_str_equal(version->version, base_version) &&
                                 (!exact || version->revision == base_revision);
                g_autoptr(MfManifest) candidate = NULL;

                if (named != (pass == 0))
                    continue;
                candidate = mfd_resolver_fetch_manifest(resolver, candidate_origin, base_id, version, error);
                if (candidate == NULL)
                    return FALSE;
                if (g_str_equal(mf_manifest_get_identity(candidate),
                                mf_manifest_get_base_identity(manifest))) {
                    *ret_item = mfd_resolver_add(resolver, candidate_origin, version, candidate, TRUE);
                    return TRUE;
                }
            }
        }
    }

    return TRUE;
}

/**
 * mfd_resolver_find_item:
 *
 * Returns: (transfer none) (nullable): the item for a version of a component,
 *   or %NULL if the set does not have it
 */
static MfdInstallItem *
mfd_resolver_find_item(MfdResolver *resolver, const char *component_id, MfRepoVersion *version)
{
    for (guint i = 0; i < resolver->set->items->len; i++) {
        MfdInstallItem *item = g_ptr_array_index(resolver->set->items, i);

        if (g_str_equal(mf_manifest_get_component_id(item->manifest), component_id) &&
            g_str_equal(mf_manifest_get_version(item->manifest), version->version) &&
            mf_manifest_get_revision(item->manifest) == version->revision)
            return item;
    }
    return NULL;
}

/**
 * mfd_resolver_add_with_bases:
 * @dependency: whether the version is only installed because another component needs it
 *
 * Add a version of a component to the set, together with the bases it needs
 * that are neither installed nor in the set.
 *
 * Returns: %TRUE if the version and its bases are in the set
 */
static gboolean
mfd_resolver_add_with_bases(MfdResolver *resolver,
                            MfdOrigin *origin,
                            const char *component_id,
                            MfRepoVersion *version,
                            gboolean dependency,
                            GError **error)
{
    MfdInstallItem *item = mfd_resolver_find_item(resolver, component_id, version);
    g_autoptr(MfManifest) manifest = NULL;

    /* the version may be in the set already, as the base of another component */
    if (item != NULL) {
        item->is_dependency = item->is_dependency && dependency;
        return TRUE;
    }

    manifest = mfd_resolver_fetch_manifest(resolver, origin, component_id, version, error);
    if (manifest == NULL)
        return FALSE;
    item = mfd_resolver_add(resolver, origin, version, manifest, dependency);

    for (guint depth = 0;; depth++) {
        MfdInstallItem *base = NULL;

        if (mf_manifest_get_base_id(item->manifest) == NULL ||
            mfd_resolver_has_base(resolver, item->manifest))
            return TRUE;
        if (depth == BASE_DEPTH_MAX) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_REPOSITORY,
                        _("%s builds on more than %u bases, one on top of the other"),
                        component_id,
                        BASE_DEPTH_MAX);
            return FALSE;
        }
        if (!mfd_resolver_find_base(resolver,
                                    mfd_origin_set_find(resolver->origin_set, item->origin),
                                    item->manifest,
                                    &base,
                                    error))
            return FALSE;
        if (base != NULL) {
            item = base;
            continue;
        }

        /* no origin has the base, so we leave it out, and the installation reports that it is missing */
        return TRUE;
    }
}

/**
 * mfd_set_error_not_found:
 * @problems: (element-type MfdOriginProblem): the remotes and channels we can not use
 *
 * Set an error for a component that no origin has. If some remotes can not
 * be used, the error lists the reasons, as one of those remotes may have the
 * component.
 */
static void
mfd_set_error_not_found(GError **error, GPtrArray *problems, const char *component_id)
{
    g_autoptr(GPtrArray) messages = g_ptr_array_new();
    g_autofree char *joined = NULL;

    if (problems->len == 0) {
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, _("No remote has %s"), component_id);
        return;
    }

    for (guint i = 0; i < problems->len; i++)
        g_ptr_array_add(messages, ((MfdOriginProblem *) g_ptr_array_index(problems, i))->error->message);
    g_ptr_array_sort_values(messages, (GCompareFunc) g_strcmp0);
    g_ptr_array_add(messages, NULL);
    joined = g_strjoinv("; ", (char **) messages->pdata);
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_NOT_FOUND,
                /* TRANSLATORS: a component, then why some remotes could not be used */
                _("No remote has %s, but some remotes could not be checked: %s"),
                component_id,
                joined);
}

/**
 * mfd_resolver_choose_origin:
 * @origin_name: (nullable): the origin that was asked for
 *
 * Find the origin to install a component from. This is the requested origin,
 * or the only origin that has the component if none was requested.
 *
 * Returns: (transfer none): the origin, or %NULL on error
 */
static MfdOrigin *
mfd_resolver_choose_origin(MfdResolver *resolver,
                           const char *component_id,
                           const char *origin_name,
                           GError **error)
{
    GPtrArray *origins = resolver->origin_set->origins;
    g_autoptr(GPtrArray) candidates = g_ptr_array_new();
    g_autoptr(GString) names = NULL;

    for (guint i = 0; i < origins->len; i++) {
        MfdOrigin *origin = g_ptr_array_index(origins, i);
        gboolean has_component = mf_repo_component_index_get_versions(origin->components, component_id) !=
                                 NULL;

        if (origin_name == NULL) {
            if (has_component)
                g_ptr_array_add(candidates, origin);
            continue;
        }
        if (!g_str_equal(origin->name, origin_name))
            continue;
        if (has_component)
            return origin;
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NOT_FOUND,
                    _("%s has no component %s"),
                    origin_name,
                    component_id);
        return NULL;
    }

    if (origin_name != NULL) {
        const GError *problem = mfd_origin_set_find_problem(resolver->origin_set, origin_name);

        /* the requested origin may be one that can not be used */
        if (problem != NULL)
            g_propagate_error(error, g_error_copy(problem));
        else
            g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, _("There is no origin %s"), origin_name);
        return NULL;
    }

    if (candidates->len == 1)
        return g_ptr_array_index(candidates, 0);
    if (candidates->len == 0) {
        mfd_set_error_not_found(error, resolver->origin_set->problems, component_id);
        return NULL;
    }

    names = g_string_new(NULL);
    for (guint i = 0; i < candidates->len; i++)
        g_string_append_printf(names,
                               "%s%s",
                               i > 0 ? ", " : "",
                               ((MfdOrigin *) g_ptr_array_index(candidates, i))->name);
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_AMBIGUOUS,
                /* TRANSLATORS: a component, then the origins that have it */
                _("Several origins have %s, choose one of them: %s"),
                component_id,
                names->str);
    return NULL;
}

/**
 * mfd_resolver_resolve_component:
 *
 * Find the newest version of the component of an install request, and the
 * bases it needs that the store does not have, and add them to the set.
 *
 * Returns: %TRUE if the component is in the set
 */
static gboolean
mfd_resolver_resolve_component(MfdResolver *resolver, MfdRepoRequest *request, GError **error)
{
    MfRepoVersion *version;
    MfdOrigin *origin;

    origin = mfd_resolver_choose_origin(resolver, request->component_id, request->origin, error);
    if (origin == NULL)
        return FALSE;
    version = g_ptr_array_index(
        mf_repo_component_index_get_versions(origin->components, request->component_id),
        0);
    g_info("Installing %s %s from %s", request->component_id, version->version, origin->name);

    return mfd_resolver_add_with_bases(resolver, origin, request->component_id, version, FALSE, error);
}

/**
 * mfd_resolver_resolve:
 * @request: the request to resolve
 * @set: an empty set, to which the versions to install are added
 * @store: the store, which has to be loaded
 * @fetcher: the fetcher to download manifests with
 * @origin_set: the origins of the remotes
 * @error: a #GError
 *
 * Find the versions that a request installs, and add them to @set with the
 * bases they need. We download and verify their manifests, but not their
 * images.
 *
 * NOTE: Even if this function fails, @set lists the remotes that manifests
 * were downloaded from, so the caller can refresh these remotes and try again.
 *
 * Returns: %TRUE if @set has what the request installs
 */
gboolean
mfd_resolver_resolve(MfdRepoRequest *request,
                     MfdInstallSet *set,
                     MfStore *store,
                     MfdFetcher *fetcher,
                     MfdOriginSet *origin_set,
                     GError **error)
{
    MfdResolver resolver = {set, store, fetcher, origin_set};

    return mfd_resolver_resolve_component(&resolver, request, error);
}
