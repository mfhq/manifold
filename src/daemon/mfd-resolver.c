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
 * components in the channels of the remotes, together with the bases they
 * need that are not installed yet. We download and verify their manifests,
 * and add them to an install set. The images are only downloaded when the
 * set is installed.
 */

/* the maximum number of layers that we resolve below one component */
constexpr guint BASE_DEPTH_MAX = 8;

/**
 * mfd_repo_request_new_install:
 * @component_id: the component to install
 * @origin: (nullable): the origin to install it from, or %NULL if only one channel has it
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
 * mfd_repo_request_new_update:
 * @component_ids: (nullable) (array zero-terminated=1): the components to update,
 *   or %NULL for every component that was installed from a remote
 *
 * Returns: (transfer full): a request to update components from their remotes
 */
MfdRepoRequest *
mfd_repo_request_new_update(const char *const *component_ids)
{
    MfdRepoRequest *request = g_new0(MfdRepoRequest, 1);

    request->update_ids = g_strdupv((GStrv) component_ids);
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
    g_strfreev(request->update_ids);
    g_free(request);
}

/* the state of resolving one request */
typedef struct {
    MfdInstallSet *set;
    MfStore *store;
    MfdFetcher *fetcher;
    MfdRemoteChannelSet *channel_set;
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
                            MfdRemoteChannel *channel,
                            const char *component_id,
                            MfRepoVersion *version,
                            GError **error)
{
    g_autofree char *url = mf_remote_build_url(channel->remote, version->manifest_location);
    g_autoptr(GBytes) data = NULL;

    mfd_install_set_add_remote(resolver->set, channel->remote);
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
                 MfdRemoteChannel *channel,
                 MfRepoVersion *version,
                 MfManifest *manifest,
                 gboolean dependency)
{
    MfdInstallItem *item = mfd_install_set_add(resolver->set, manifest, version->bundle, channel->origin);

    item->is_dependency = dependency;
    mfd_repo_image_set_source(item, resolver->store, resolver->fetcher, channel, version);
    return item;
}

/**
 * mfd_resolver_find_base:
 * @channel: the channel of the component, which is searched first
 * @ref: the reference to the base or base SDK of the component
 * @ret_item: (out) (transfer none): the item of the base, or %NULL if no channel has it
 *
 * Find a base or base SDK of a component in the channels of the remotes,
 * and add it to the set.
 *
 * An exact reference only matches the referenced version and revision. Any
 * other reference matches every version with the referenced identity, and
 * the referenced version is tried first. The identity of a version is only
 * recorded in its manifest, so we download manifests until one matches.
 *
 * Returns: %FALSE if a manifest could not be downloaded or verified
 */
static gboolean
mfd_resolver_find_base(MfdResolver *resolver,
                       MfdRemoteChannel *channel,
                       const MfBaseReference *ref,
                       MfdInstallItem **ret_item,
                       GError **error)
{
    GPtrArray *channels = resolver->channel_set->channels;
    g_autoptr(GPtrArray) order = g_ptr_array_new();

    *ret_item = NULL;
    g_ptr_array_add(order, channel);
    for (guint i = 0; i < channels->len; i++) {
        if (g_ptr_array_index(channels, i) != channel)
            g_ptr_array_add(order, g_ptr_array_index(channels, i));
    }

    for (guint i = 0; i < order->len; i++) {
        MfdRemoteChannel *candidate_channel = g_ptr_array_index(order, i);
        GPtrArray *versions = mf_repo_component_index_get_versions(candidate_channel->components, ref->id);

        /* the first pass tries the named version, the second pass all other versions, newest first */
        for (guint pass = 0; versions != NULL && pass < (ref->exact ? 1 : 2); pass++) {
            for (guint j = 0; j < versions->len; j++) {
                MfRepoVersion *version = g_ptr_array_index(versions, j);
                gboolean named = g_str_equal(version->version, ref->version) &&
                                 (!ref->exact || version->revision == ref->revision);
                g_autoptr(MfManifest) candidate = NULL;

                if (named != (pass == 0))
                    continue;
                candidate = mfd_resolver_fetch_manifest(resolver, candidate_channel, ref->id, version, error);
                if (candidate == NULL)
                    return FALSE;
                if (g_str_equal(mf_manifest_get_identity(candidate), ref->identity)) {
                    *ret_item = mfd_resolver_add(resolver, candidate_channel, version, candidate, TRUE);
                    return TRUE;
                }
            }
        }
    }

    return TRUE;
}

/**
 * mfd_resolver_add_bases:
 * @item: an item of the set
 * @depth: how many layers @item is below the requested component
 * @need_bases: %TRUE to fail if no channel has a needed base, %FALSE to leave
 *   the base out, so the installation reports that it is missing
 *
 * Add the base and the base SDK of an item to the set, unless they are
 * installed or in the set already. We then do the same for every component
 * that was added.
 *
 * Returns: %TRUE if all bases of @item are installed or in the set, or if a
 *   missing base was ignored because @need_bases is %FALSE
 */
static gboolean
mfd_resolver_add_bases(MfdResolver *resolver,
                       MfdInstallItem *item,
                       guint depth,
                       gboolean need_bases,
                       GError **error)
{
    const MfBaseReference *refs[] = {
        mf_manifest_get_base(item->manifest),
        mf_manifest_get_base_sdk(item->manifest),
    };

    for (guint i = 0; i < G_N_ELEMENTS(refs); i++) {
        const MfBaseReference *ref = refs[i];
        g_auto(MfdBaseProvider) provider = {};
        MfdInstallItem *base = NULL;

        /* check if the referenced component is installed, or is in the set already */
        if (ref == NULL || mfd_install_set_find_base(resolver->set, resolver->store, ref, &provider))
            continue;
        if (depth == BASE_DEPTH_MAX) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_REPOSITORY,
                        _("%s is more than %u layers below the component that needs it"),
                        mf_manifest_get_component_id(item->manifest),
                        BASE_DEPTH_MAX);
            return FALSE;
        }
        if (!mfd_resolver_find_base(resolver,
                                    mfd_remote_channel_set_find(resolver->channel_set, item->origin),
                                    ref,
                                    &base,
                                    error))
            return FALSE;
        if (base != NULL) {
            if (!mfd_resolver_add_bases(resolver, base, depth + 1, need_bases, error))
                return FALSE;
            continue;
        }

        if (!need_bases)
            continue;
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_MISSING_BASE,
                    /* TRANSLATORS: a component and its version, then its base and the version of the base */
                    _("%s %s needs the base %s %s, which is not available in any remote"),
                    mf_manifest_get_component_id(item->manifest),
                    mf_manifest_get_version(item->manifest),
                    ref->id,
                    ref->version);
        return FALSE;
    }

    return TRUE;
}

/**
 * mfd_resolver_add_with_bases:
 * @dependency: whether the version is only installed because another component needs it
 * @need_bases: %TRUE to fail if no channel has a needed base, %FALSE to leave
 *   the base out, so the installation reports that it is missing
 *
 * Add a version of a component to the set, together with the bases it needs
 * that are neither installed nor in the set.
 *
 * Returns: %TRUE if the version and its bases are in the set
 */
static gboolean
mfd_resolver_add_with_bases(MfdResolver *resolver,
                            MfdRemoteChannel *channel,
                            const char *component_id,
                            MfRepoVersion *version,
                            gboolean dependency,
                            gboolean need_bases,
                            GError **error)
{
    MfdInstallItem *item = mfd_install_set_find_version(resolver->set,
                                                        component_id,
                                                        version->version,
                                                        version->revision);
    g_autoptr(MfManifest) manifest = NULL;

    /* the version may be in the set already, as the base of another component */
    if (item != NULL) {
        item->is_dependency = item->is_dependency && dependency;
        return TRUE;
    }

    manifest = mfd_resolver_fetch_manifest(resolver, channel, component_id, version, error);
    if (manifest == NULL)
        return FALSE;
    item = mfd_resolver_add(resolver, channel, version, manifest, dependency);

    return mfd_resolver_add_bases(resolver, item, 0, need_bases, error);
}

/**
 * mfd_set_error_not_found:
 * @problems: (element-type MfdRemoteProblem): the remotes and channels we can not use
 *
 * Set an error for a component that no remote has. If some remotes can not
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
        g_ptr_array_add(messages, ((MfdRemoteProblem *) g_ptr_array_index(problems, i))->error->message);
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
 * mfd_resolver_choose_channel:
 * @origin: (nullable): the origin that was asked for
 *
 * Find the channel to install a component from. This is the channel with the
 * requested origin, or the only channel that has the component if no origin
 * was requested.
 *
 * Returns: (transfer none): the channel, or %NULL on error
 */
static MfdRemoteChannel *
mfd_resolver_choose_channel(MfdResolver *resolver,
                            const char *component_id,
                            const char *origin,
                            GError **error)
{
    GPtrArray *channels = resolver->channel_set->channels;
    g_autoptr(GPtrArray) candidates = g_ptr_array_new();
    g_autoptr(GString) names = NULL;

    for (guint i = 0; i < channels->len; i++) {
        MfdRemoteChannel *channel = g_ptr_array_index(channels, i);
        gboolean has_component = mf_repo_component_index_get_versions(channel->components, component_id) !=
                                 NULL;

        if (origin == NULL) {
            if (has_component)
                g_ptr_array_add(candidates, channel);
            continue;
        }
        if (!g_str_equal(channel->origin, origin))
            continue;
        if (has_component)
            return channel;
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, _("%s has no component %s"), origin, component_id);
        return NULL;
    }

    if (origin != NULL) {
        const GError *problem = mfd_remote_channel_set_find_problem(resolver->channel_set, origin);

        /* the requested channel may be one that can not be used */
        if (problem != NULL)
            g_propagate_error(error, g_error_copy(problem));
        else
            g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, _("There is no origin %s"), origin);
        return NULL;
    }

    if (candidates->len == 1)
        return g_ptr_array_index(candidates, 0);
    if (candidates->len == 0) {
        mfd_set_error_not_found(error, resolver->channel_set->problems, component_id);
        return NULL;
    }

    names = g_string_new(NULL);
    for (guint i = 0; i < candidates->len; i++)
        g_string_append_printf(names,
                               "%s%s",
                               i > 0 ? ", " : "",
                               ((MfdRemoteChannel *) g_ptr_array_index(candidates, i))->origin);
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
    MfdRemoteChannel *channel;

    channel = mfd_resolver_choose_channel(resolver, request->component_id, request->origin, error);
    if (channel == NULL)
        return FALSE;
    version = g_ptr_array_index(
        mf_repo_component_index_get_versions(channel->components, request->component_id),
        0);
    g_info("Installing %s %s from %s", request->component_id, version->version, channel->origin);

    return mfd_resolver_add_with_bases(resolver,
                                       channel,
                                       request->component_id,
                                       version,
                                       FALSE,
                                       FALSE,
                                       error);
}

/* an update of a component to the newest version in the channel it came from */
typedef struct {
    MfComponent *component; /* the installed version */
    MfdRemoteChannel *channel;
    MfRepoVersion *version; /* owned by the component index of @channel */
} MfdUpdate;

static void
mfd_update_free(MfdUpdate *update)
{
    g_object_unref(update->component);
    mfd_remote_channel_unref(update->channel);
    g_free(update);
}

/**
 * mfd_component_is_dependency:
 *
 * Returns: %TRUE if @component is a runtime that was installed as a dependency
 *   of another component
 */
static gboolean
mfd_component_is_dependency(MfComponent *component)
{
    return mf_component_get_install_mode(component) == MF_INSTALL_MODE_AUTO &&
           !mf_component_kind_is_always_auto(mf_component_get_kind(component),
                                             mf_component_is_sdk(component));
}

/**
 * mfd_find_followed_bases:
 * @installed: (element-type MfComponent): every installed version
 *
 * Returns: (transfer full) (element-type utf8): the IDs of all components
 *   that an installed version uses as its base through the `current` link
 */
static GHashTable *
mfd_find_followed_bases(GPtrArray *installed)
{
    GHashTable *followed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < installed->len; i++) {
        g_autoptr(MfBaseLink) link = mf_store_read_base_link(g_ptr_array_index(installed, i), NULL);

        if (link != NULL && link->version_name == NULL)
            g_hash_table_add(followed, g_strdup(link->component_id));
    }
    return followed;
}

/**
 * mfd_find_current:
 *
 * Returns: (transfer none) (nullable): the current version of a component
 */
static MfComponent *
mfd_find_current(GPtrArray *installed, const char *component_id)
{
    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);

        if (mf_component_is_current(component) && g_str_equal(mf_component_get_id(component), component_id))
            return component;
    }
    return NULL;
}

/**
 * mfd_check_update:
 * @warnings: (element-type utf8): the list to add a warning to if @component can not be updated
 *
 * Check whether the channel that a component came from has a newer version
 * of it.
 *
 * Returns: (transfer full) (nullable): the update, or %NULL if there is none
 */
static MfdUpdate *
mfd_check_update(MfdRemoteChannelSet *channel_set, MfComponent *component, GPtrArray *warnings)
{
    const char *component_id = mf_component_get_id(component);
    const char *origin = mf_component_get_origin(component);
    MfdRemoteChannel *channel = mfd_remote_channel_set_find(channel_set, origin);
    MfdUpdate *update;
    MfRepoVersion *newest;
    GPtrArray *versions;

    if (channel == NULL) {
        const GError *problem = mfd_remote_channel_set_find_problem(channel_set, origin);

        if (problem != NULL)
            g_ptr_array_add(warnings,
                            g_strdup_printf(_("Unable to update %s: %s"), component_id, problem->message));
        else
            g_ptr_array_add(warnings,
                            g_strdup_printf(_("%s was installed from %s, which is no longer configured"),
                                            component_id,
                                            origin));
        return NULL;
    }
    versions = mf_repo_component_index_get_versions(channel->components, component_id);
    if (versions == NULL) {
        g_ptr_array_add(warnings, g_strdup_printf(_("%s no longer has %s"), origin, component_id));
        return NULL;
    }
    newest = g_ptr_array_index(versions, 0);
    if (mf_version_compare_with_rev(newest->version,
                                    newest->revision,
                                    mf_component_get_version(component),
                                    mf_component_get_revision(component)) <= 0)
        return NULL;

    update = g_new0(MfdUpdate, 1);
    update->component = g_object_ref(component);
    update->channel = mfd_remote_channel_ref(channel);
    update->version = newest;
    return update;
}

/**
 * mfd_resolver_find_updates:
 * @component_ids: (nullable) (array zero-terminated=1): the components to
 *   update, or %NULL for every component from a remote
 * @warnings: (element-type utf8): the list to add warnings to about components
 *   that can not be updated
 * @error: a #GError
 *
 * Find the components that have a newer version in the channel they came from.
 *
 * Without @component_ids we check every component that was installed from a
 * remote. We skip runtimes that were installed as a dependency, unless an
 * installed component uses them through their `current` link. A newer version
 * of such a runtime would not be used by anything, and the next cleanup would
 * remove it again.
 *
 * Returns: (transfer full) (element-type MfdUpdate): the updates, or %NULL if
 *   one of @component_ids is not installed, or not from a remote
 */
static GPtrArray *
mfd_resolver_find_updates(MfdResolver *resolver,
                          const char *const *component_ids,
                          GPtrArray *warnings,
                          GError **error)
{
    g_autoptr(GPtrArray)
        installed = mf_store_list_components(resolver->store, NULL, MF_COMPONENT_KIND_UNKNOWN, error);
    g_autoptr(GPtrArray) candidates = g_ptr_array_new();
    g_autoptr(GPtrArray) updates = NULL;

    if (installed == NULL)
        return NULL;

    if (component_ids != NULL) {
        for (guint i = 0; component_ids[i] != NULL; i++) {
            MfComponent *component = mfd_find_current(installed, component_ids[i]);

            if (component == NULL) {
                g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, _("%s is not installed"), component_ids[i]);
                return NULL;
            }
            if (g_str_equal(mf_component_get_origin(component), MF_BUNDLE_CATALOG_ORIGIN)) {
                g_set_error(error,
                            MF_ERROR,
                            MF_ERROR_NOT_FOUND,
                            _("%s was installed from a bundle, so no remote has updates for it"),
                            component_ids[i]);
                return NULL;
            }
            g_ptr_array_add(candidates, component);
        }
    } else {
        g_autoptr(GHashTable) followed = mfd_find_followed_bases(installed);

        for (guint i = 0; i < installed->len; i++) {
            MfComponent *component = g_ptr_array_index(installed, i);

            if (!mf_component_is_current(component) ||
                g_str_equal(mf_component_get_origin(component), MF_BUNDLE_CATALOG_ORIGIN))
                continue;
            if (mfd_component_is_dependency(component) &&
                !g_hash_table_contains(followed, mf_component_get_id(component)))
                continue;
            g_ptr_array_add(candidates, component);
        }
    }

    updates = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_update_free);
    for (guint i = 0; i < candidates->len; i++) {
        MfdUpdate *update = mfd_check_update(resolver->channel_set,
                                             g_ptr_array_index(candidates, i),
                                             warnings);

        if (update != NULL)
            g_ptr_array_add(updates, update);
    }

    return g_steal_pointer(&updates);
}

/**
 * mfd_resolver_resolve_updates:
 * @updates: (element-type MfdUpdate): the updates to add to the set
 * @failures: (element-type GError): the list to add an error to for each
 *   update that could not be resolved
 *
 * Add the new versions of the updates to the set, with the bases they need
 * that are not installed. An update that can not be resolved, e.g. because
 * a manifest could not be downloaded, is left out, so the other updates are
 * still installed. A runtime that was installed as a dependency stays
 * a dependency.
 */
static void
mfd_resolver_resolve_updates(MfdResolver *resolver, GPtrArray *updates, GPtrArray *failures)
{
    MfdInstallSet *set = resolver->set;

    for (guint i = 0; i < updates->len; i++) {
        MfdUpdate *update = g_ptr_array_index(updates, i);
        const char *component_id = mf_component_get_id(update->component);
        guint n_items = set->items->len;
        g_autoptr(GError) error = NULL;

        g_info("Updating %s %s to %s from %s",
               component_id,
               mf_component_get_full_version(update->component),
               update->version->version,
               update->channel->origin);
        if (mfd_resolver_add_with_bases(resolver,
                                        update->channel,
                                        component_id,
                                        update->version,
                                        mfd_component_is_dependency(update->component),
                                        TRUE,
                                        &error))
            continue;

        /* remove the items that the failed update added to the set */
        g_ptr_array_set_size(set->items, n_items);
        g_ptr_array_add(failures,
                        g_error_new(error->domain,
                                    error->code,
                                    _("Unable to update %s: %s"),
                                    component_id,
                                    error->message));
    }
}

/**
 * mfd_resolver_resolve:
 * @request: the request to resolve
 * @set: an empty set, to which the versions to install are added
 * @store: the store, which has to be loaded
 * @fetcher: the fetcher to download manifests with
 * @channel_set: the channels of the remotes
 * @failures: (element-type GError): the list to add an error to for each
 *   update that could not be resolved
 * @error: a #GError
 *
 * Find the versions that a request installs, and add them to @set with the
 * bases they need. We download and verify their manifests, but not their
 * images. A component that an update request can not update is left out,
 * and @set has a warning about it.
 *
 * NOTE: Even if this function fails, @set lists the remotes that manifests
 * were downloaded from, so the caller can refresh these remotes and try again.
 *
 * Returns: %TRUE if the versions to install were added to @set. An update
 *   request adds no versions if no component has an update.
 */
gboolean
mfd_resolver_resolve(MfdRepoRequest *request,
                     MfdInstallSet *set,
                     MfStore *store,
                     MfdFetcher *fetcher,
                     MfdRemoteChannelSet *channel_set,
                     GPtrArray *failures,
                     GError **error)
{
    MfdResolver resolver = {set, store, fetcher, channel_set};
    g_autoptr(GPtrArray) updates = NULL;

    if (request->component_id != NULL)
        return mfd_resolver_resolve_component(&resolver, request, error);

    updates = mfd_resolver_find_updates(&resolver,
                                        (const char *const *) request->update_ids,
                                        set->warnings,
                                        error);
    if (updates == NULL)
        return FALSE;
    mfd_resolver_resolve_updates(&resolver, updates, failures);

    return TRUE;
}
