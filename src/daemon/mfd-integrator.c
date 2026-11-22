/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-integrator.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <gio/gio.h>
#include <libglnx.h>
#include <systemd/sd-json.h>

#include "mf-integration-private.h"
#include "mf-json-private.h"
#include "mf-sdautoptr-private.h"
#include "mf-store-private.h"
#include "mfd-image-reader.h"
#include "mfd-utils.h"

/*
 * Helper for exporting the integration data of components.
 *
 * Every component has a registry that lists the files we installed for it on
 * the host, and a component owns the files that its registry lists. We replace
 * and remove owned files without looking at them first, so an application
 * always gets the files it expects. Admins can still override the owned files
 * in directories that take precedence over the directories of Manifold. A file
 * that no registry lists does not belong to Manifold: we never change it, and
 * we refuse to install a component that wants to install a file at its path.
 *
 * Before we change any file, we add the new files to the registry, next to the
 * old ones. After a crash, the registry therefore lists every file that we may
 * have left behind, and the next operation completes the update.
 *
 * We never enable a systemd unit, and we never start one that was not running.
 * Whether a service runs on a host is up to the admin of that host.
 */

/* timeout for a host tool, in case it hangs */
constexpr guint HOST_TOOL_TIMEOUT_SEC = 60;

/* file modes of the exported files and of the aliases */
constexpr int EXPORT_FILE_MODE = 0644;
constexpr int EXPORT_ALIAS_MODE = 0755;

/* The directories that systemd and udev search in addition to our prefix. Each of them has
 * the same layout as the `lib` directory of our prefix. The last three directories are only
 * searched for user units, they are the defaults of $XDG_CONFIG_DIRS and $XDG_DATA_DIRS. */
static const char *const HOST_SEARCH_DIRS[] = {
    "/etc",
    "/run",
    "/usr/lib",
    "/etc/xdg",
    "/usr/share",
    MF_INTEGRATION_PREFIX "/share",
    NULL,
};

struct _MfdIntegrator {
    GObject parent_instance;

    MfStore *store;
    MfdStoreWriter *writer;
};

G_DEFINE_FINAL_TYPE(MfdIntegrator, mfd_integrator, G_TYPE_OBJECT)

static void
mfd_integrator_finalize(GObject *object)
{
    MfdIntegrator *integrator = MFD_INTEGRATOR(object);

    g_clear_object(&integrator->store);
    g_clear_object(&integrator->writer);

    G_OBJECT_CLASS(mfd_integrator_parent_class)->finalize(object);
}

static void
mfd_integrator_class_init(MfdIntegratorClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mfd_integrator_finalize;
}

static void
mfd_integrator_init(MfdIntegrator *integrator)
{
}

/**
 * mfd_integrator_new:
 * @store: the store with the components to integrate
 * @writer: the writer of @store
 *
 * Returns: (transfer full): a new #MfdIntegrator
 */
MfdIntegrator *
mfd_integrator_new(MfStore *store, MfdStoreWriter *writer)
{
    MfdIntegrator *integrator = g_object_new(MFD_TYPE_INTEGRATOR, NULL);

    integrator->store = g_object_ref(store);
    integrator->writer = g_object_ref(writer);

    return integrator;
}

/**
 * MfdExport:
 *
 * A file that a component has installed on the host, or is about to install.
 * It is either a regular file or a symbolic link. The registry records the
 * checksum or the link target, so that repairing an installation can find the
 * files that were changed.
 */
typedef struct {
    char *path;             /* the path of the file on the host */
    char *sha256;           /* the checksum of a regular file, or %NULL for a link */
    char *target;           /* the target of a link, or %NULL for a regular file */
    GBytes *content;        /* the content to write to a regular file */
    int mode;               /* the permissions of a regular file */
    char *dir;              /* the host directory for files of this kind, which @path must be below */
    MfIntegrationKind kind; /* the kind of the file, %MF_INTEGRATION_KIND_UNKNOWN for an alias */
} MfdExport;

static void
mfd_export_free(MfdExport *export)
{
    g_free(export->path);
    g_free(export->sha256);
    g_free(export->target);
    g_clear_pointer(&export->content, g_bytes_unref);
    g_free(export->dir);
    g_free(export);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdExport, mfd_export_free)

/**
 * mfd_exports_new:
 *
 * Returns: (transfer full) (element-type utf8 MfdExport): a table of path -> MfdExport
 */
static GHashTable *
mfd_exports_new(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify) mfd_export_free);
}

/**
 * mfd_exports_add:
 *
 * Add a file to a table, replacing a file with the same path.
 */
static void
mfd_exports_add(GHashTable *exports, MfdExport *export)
{
    /* the key is owned by the old value, so we have to remove the old entry before adding the new one */
    g_hash_table_remove(exports, export->path);
    g_hash_table_insert(exports, export->path, export);
}

/**
 * mfd_export_install:
 *
 * Install a file on the host, replacing an existing file at the same path.
 *
 * We create the host directory for the kind of the file if it does not exist.
 * Only icons may be in a subdirectory of that directory, as icons are sorted
 * by size and category. A file is never installed outside of the directory
 * for its kind, see mfd_make_dir_below().
 */
static gboolean
mfd_export_install(const MfdExport *export, GError **error)
{
    g_autofree char *subdir = NULL;
    g_autofree char *name = NULL;
    g_autofd int dfd = -1;
    g_autofd int subdir_fd = -1;

    if (export->dir == NULL || !mf_path_is_below(export->path, export->dir))
        return glnx_throw(error,
                          "Not installing %s, as it is not in the directory of its kind",
                          export->path);
    subdir = g_path_get_dirname(export->path + strlen(export->dir) + 1);
    name = g_path_get_basename(export->path);
    if (!g_str_equal(subdir, ".") && export->kind != MF_INTEGRATION_KIND_ICON)
        return glnx_throw(error,
                          "Not installing %s, as only icons may be installed in subdirectories",
                          export->path);

    if (!mfd_make_dir(export->dir, error) || !glnx_opendirat(AT_FDCWD, export->dir, TRUE, &dfd, error))
        return FALSE;
    subdir_fd = mfd_make_dir_below(dfd, subdir, error);
    if (subdir_fd < 0)
        return FALSE;

    if (export->target != NULL)
        return mfd_replace_link_at(subdir_fd, name, export->target, error);
    return mfd_write_file_at(subdir_fd,
                             name,
                             g_bytes_get_data(export->content, NULL),
                             g_bytes_get_size(export->content),
                             export->mode,
                             error);
}

/**
 * MfdRegistry:
 *
 * The registry of a component, which lists the files that we installed on the host for it.
 */
typedef struct {
    char *path;          /* the path of the registry file */
    char *version;       /* the full version that the files belong to, or %NULL while we change the files */
    GHashTable *exports; /* (element-type utf8 MfdExport) the files by their path */
} MfdRegistry;

static void
mfd_registry_free(MfdRegistry *registry)
{
    g_free(registry->path);
    g_free(registry->version);
    g_hash_table_unref(registry->exports);
    g_free(registry);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdRegistry, mfd_registry_free)

/**
 * mfd_kind_is_in_prefix:
 *
 * Returns: %TRUE if files of this kind are installed below our prefix. The search
 *   paths of systemd and udev are fixed, so their files can not be installed in
 *   the exports directory of the store.
 */
static gboolean
mfd_kind_is_in_prefix(MfIntegrationKind kind)
{
    switch (kind) {
    case MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM:
    case MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER:
    case MF_INTEGRATION_KIND_UDEV_RULE:
        return TRUE;
    default:
        return FALSE;
    }
}

/**
 * mfd_integrator_path_is_export:
 * @path: a path that a registry lists
 *
 * Returns: %TRUE if @path is in a directory that we install files to
 */
static gboolean
mfd_integrator_path_is_export(MfdIntegrator *integrator, const char *path)
{
    g_autofree char *exports = g_build_filename(mf_store_get_path(integrator->store),
                                                MF_STORE_EXPORTS_NAME,
                                                NULL);

    if (mf_path_is_below(path, exports))
        return TRUE;
    for (MfIntegrationKind kind = MF_INTEGRATION_KIND_UNKNOWN + 1; kind < MF_INTEGRATION_KIND_LAST; kind++) {
        g_autofree char *dir = g_build_filename(MF_INTEGRATION_PREFIX,
                                                mf_integration_kind_get_dir(kind),
                                                NULL);

        if (mfd_kind_is_in_prefix(kind) && mf_path_is_below(path, dir))
            return TRUE;
    }

    return FALSE;
}

/**
 * mfd_integrator_read_registry:
 *
 * Read the registry of a component. An entry with a path outside of the
 * directories that we install files to is ignored, so a manipulated registry
 * can not make us replace or remove any other file.
 *
 * Returns: (transfer full): the registry of a component, which is empty if the
 *   component has none, or %NULL on error
 */
static MfdRegistry *
mfd_integrator_read_registry(MfdIntegrator *integrator, const char *component_id, GError **error)
{
    g_autofree char *name = g_strconcat(component_id, ".json", NULL);
    g_autoptr(MfdRegistry) registry = g_new0(MfdRegistry, 1);
    g_autoptr(sd_json_variant) json = NULL;
    sd_json_variant *files;

    registry->path = g_build_filename(mf_store_get_path(integrator->store),
                                      MF_STORE_INTEGRATIONS_SUBDIR,
                                      name,
                                      NULL);
    registry->exports = mfd_exports_new();
    if (!g_file_test(registry->path, G_FILE_TEST_EXISTS))
        return g_steal_pointer(&registry);

    json = mf_json_parse_file(registry->path, error);
    if (json == NULL)
        return NULL;
    registry->version = g_strdup(mf_json_get_string(json, "version"));

    files = sd_json_variant_by_key(json, "files");
    for (size_t i = 0; i < sd_json_variant_elements(files); i++) {
        sd_json_variant *entry = sd_json_variant_by_index(files, i);
        MfdExport *export = g_new0(MfdExport, 1);

        export->path = g_strdup(mf_json_get_string(entry, "path"));
        export->sha256 = g_strdup(mf_json_get_string(entry, "sha256"));
        export->target = g_strdup(mf_json_get_string(entry, "target"));
        if (export->path == NULL || !mfd_integrator_path_is_export(integrator, export->path)) {
            g_warning("Ignoring %s in the registry of %s, as no files are installed in this location",
                      export->path != NULL ? export->path : "an entry without a path",
                      component_id);
            mfd_export_free(export);
            continue;
        }
        mfd_exports_add(registry->exports, export);
    }

    return g_steal_pointer(&registry);
}

/**
 * mfd_registry_save:
 *
 * Write a registry. The files are sorted by path, so the registry file only
 * changes if the list of files changes.
 */
static gboolean
mfd_registry_save(MfdRegistry *registry, GError **error)
{
    g_autoptr(GList) paths = g_list_sort(g_hash_table_get_keys(registry->exports), (GCompareFunc) g_strcmp0);
    g_autoptr(sd_json_variant) files = NULL;
    g_autoptr(sd_json_variant) json = NULL;
    g_autofree char *dir = g_path_get_dirname(registry->path);
    int r;

    r = sd_json_variant_new_array(&files, NULL, 0);
    for (const GList *l = paths; r >= 0 && l != NULL; l = l->next) {
        const MfdExport *export = g_hash_table_lookup(registry->exports, l->data);

        r = sd_json_variant_append_arraybo(
            &files,
            SD_JSON_BUILD_PAIR_STRING("path", export->path),
            SD_JSON_BUILD_PAIR_CONDITION(export->sha256 != NULL,
                                         "sha256",
                                         SD_JSON_BUILD_STRING(export->sha256)),
            SD_JSON_BUILD_PAIR_CONDITION(export->target != NULL,
                                         "target",
                                         SD_JSON_BUILD_STRING(export->target)));
    }
    if (r >= 0)
        r = sd_json_buildo(&json,
                           SD_JSON_BUILD_PAIR_CONDITION(registry->version != NULL,
                                                        "version",
                                                        SD_JSON_BUILD_STRING(registry->version)),
                           SD_JSON_BUILD_PAIR_VARIANT("files", files));
    if (r < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "Unable to build %s: %s",
                    registry->path,
                    g_strerror(-r));
        return FALSE;
    }

    if (!mfd_make_dir(dir, error))
        return FALSE;
    return mf_json_write_file(registry->path, json, error);
}

/**
 * mfd_integrator_add_registered_ids:
 * @ids: (element-type utf8): the set to add to
 *
 * Add the IDs of all components that have a registry. These components may not
 * be installed anymore.
 */
static void
mfd_integrator_add_registered_ids(MfdIntegrator *integrator, GHashTable *ids)
{
    g_autofree char *dir_path = g_build_filename(mf_store_get_path(integrator->store),
                                                 MF_STORE_INTEGRATIONS_SUBDIR,
                                                 NULL);
    g_autoptr(GDir) dir = g_dir_open(dir_path, 0, NULL);
    const char *name;

    while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
        g_autofree char *id = NULL;

        if (!g_str_has_suffix(name, ".json"))
            continue;
        id = g_strndup(name, strlen(name) - strlen(".json"));
        if (mf_component_id_is_valid(id))
            g_hash_table_add(ids, g_steal_pointer(&id));
    }
}

/**
 * mfd_integrator_read_owners:
 *
 * Read the registries of all components.
 *
 * Returns: (transfer full) (element-type utf8 utf8): a table of path -> ID of the
 *   component that owns the file, or %NULL on error
 */
static GHashTable *
mfd_integrator_read_owners(MfdIntegrator *integrator, GError **error)
{
    g_autoptr(GHashTable) owners = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(GHashTable) ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTableIter iter;
    gpointer id;

    mfd_integrator_add_registered_ids(integrator, ids);

    g_hash_table_iter_init(&iter, ids);
    while (g_hash_table_iter_next(&iter, &id, NULL)) {
        g_autoptr(MfdRegistry) registry = mfd_integrator_read_registry(integrator, id, error);
        GHashTableIter exports_iter;
        gpointer path;

        if (registry == NULL)
            return NULL;

        g_hash_table_iter_init(&exports_iter, registry->exports);
        while (g_hash_table_iter_next(&exports_iter, &path, NULL))
            g_hash_table_replace(owners, g_strdup(path), g_strdup(id));
    }

    return g_steal_pointer(&owners);
}

/**
 * mfd_find_host_file:
 * @path: the path that a component wants to install a file at
 *
 * Find a file in the search directories of the host that has the same name as
 * a file below our prefix. Systemd and udev only use the first file with a
 * given name that they find, so our file would either override the file of
 * the host, or have no effect.
 *
 * Returns: (transfer full) (nullable): the path of the conflicting file of the host, or %NULL
 */
static char *
mfd_find_host_file(const char *path)
{
    const char *lib_dir = MF_INTEGRATION_PREFIX "/lib/";

    if (!g_str_has_prefix(path, lib_dir))
        return NULL;

    for (guint i = 0; HOST_SEARCH_DIRS[i] != NULL; i++) {
        g_autofree char *host_file = g_build_filename(HOST_SEARCH_DIRS[i], path + strlen(lib_dir), NULL);

        if (g_file_test(host_file, G_FILE_TEST_EXISTS) || mfd_is_symlink(host_file))
            return g_steal_pointer(&host_file);
    }

    return NULL;
}

/**
 * mfd_owners_check_path:
 * @owners: (element-type utf8 utf8): a table of path -> ID of the owning component
 * @path: the path that a component wants to install a file at
 * @component_id: the component that wants to install the file
 *
 * Check that a component may install a file at a path. This is allowed if the
 * component owns the file at this path already, or if no file exists at the
 * path and the host has no file with the same name in its search directories.
 *
 * Returns: %TRUE if the component may install the file
 */
static gboolean
mfd_owners_check_path(GHashTable *owners, const char *path, const char *component_id, GError **error)
{
    const char *owner = g_hash_table_lookup(owners, path);
    g_autofree char *host_file = NULL;

    if (g_strcmp0(owner, component_id) == 0)
        return TRUE;
    if (owner != NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FILE_CONFLICT,
                    "%s needs to install %s, but that file belongs to %s",
                    component_id,
                    path,
                    owner);
        return FALSE;
    }
    if (g_file_test(path, G_FILE_TEST_EXISTS) || mfd_is_symlink(path)) {
        g_set_error(
            error,
            MF_ERROR,
            MF_ERROR_FILE_CONFLICT,
            "%s needs to install %s, but that file already exists and does not belong to any component",
            component_id,
            path);
        return FALSE;
    }
    host_file = mfd_find_host_file(path);
    if (host_file != NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FILE_CONFLICT,
                    "%s needs to install %s, but the host already has %s",
                    component_id,
                    path,
                    host_file);
        return FALSE;
    }

    return TRUE;
}

/**
 * mfd_integrator_find_current:
 * @ret_component: (out) (transfer full) (nullable): the version whose integration
 *   files have to be installed on the host, or %NULL if the component must not
 *   have any exports
 *
 * Find the version of a component whose integration files have to be installed
 * on the host. This is the current version, unless the component was revoked,
 * as a revoked component must not be launched anymore.
 *
 * Returns: %TRUE on success, even if there is no such version
 */
static gboolean
mfd_integrator_find_current(MfdIntegrator *integrator,
                            const char *component_id,
                            MfComponent **ret_component,
                            GError **error)
{
    g_autoptr(GError) local_error = NULL;

    *ret_component = NULL;
    if (mf_store_component_is_revoked_at(mf_store_get_path(integrator->store), component_id))
        return TRUE;

    *ret_component = mf_store_get_component(integrator->store, component_id, NULL, &local_error);
    if (*ret_component == NULL && !g_error_matches(local_error, MF_ERROR, MF_ERROR_NOT_FOUND)) {
        g_propagate_error(error, g_steal_pointer(&local_error));
        return FALSE;
    }

    return TRUE;
}

/**
 * mfd_integrator_read_exports:
 *
 * Read the integration files of a version from the `exports` directory of its
 * version directory. If a file is missing or damaged, we extract all files
 * from the image again.
 *
 * Returns: (transfer full) (element-type utf8 GBytes): the contents of the
 *   files by their export path, or %NULL on error
 */
static GHashTable *
mfd_integrator_read_exports(MfdIntegrator *integrator,
                            MfComponent *component,
                            MfManifest *manifest,
                            GError **error)
{
    GPtrArray *files = mf_integration_get_files(mf_manifest_get_integration(manifest));
    g_autoptr(GHashTable) exports = NULL;

    exports = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
    for (guint i = 0; i < files->len; i++) {
        MfIntegrationFile *file = g_ptr_array_index(files, i);
        const char *export_path = mf_integration_file_get_export_path(file);
        g_autofree char *path = g_build_filename(mf_component_get_dir(component),
                                                 MF_STORE_EXPORTS_NAME,
                                                 export_path,
                                                 NULL);
        g_autoptr(GBytes) content = NULL;
        char *data = NULL;
        gsize length;

        if (g_file_get_contents(path, &data, &length, NULL))
            content = g_bytes_new_take(data, length);
        if (content == NULL || !mf_integration_file_check_content(file, content)) {
            g_hash_table_unref(g_steal_pointer(&exports));
            exports = mfd_image_read_integration(integrator->store, manifest, error);
            if (exports == NULL ||
                !mfd_store_writer_write_exports(integrator->writer, component, exports, error))
                return NULL;
            return g_steal_pointer(&exports);
        }
        g_hash_table_insert(exports, g_strdup(export_path), g_steal_pointer(&content));
    }

    return g_steal_pointer(&exports);
}

/**
 * mfd_integrator_build_host_path:
 * @export_path: a path relative to the exports directory of the store
 *
 * Returns: (transfer full): the path of the file in the exports directory of the store
 */
static char *
mfd_integrator_build_host_path(MfdIntegrator *integrator, const char *export_path)
{
    return g_build_filename(mf_store_get_path(integrator->store), MF_STORE_EXPORTS_NAME, export_path, NULL);
}

/**
 * mfd_integrator_build_file_path:
 *
 * Returns: (transfer full): the path on the host that an integration file is installed at
 */
static char *
mfd_integrator_build_file_path(MfdIntegrator *integrator, MfIntegrationFile *file)
{
    const char *export_path = mf_integration_file_get_export_path(file);

    if (mfd_kind_is_in_prefix(mf_integration_file_get_kind(file)))
        return g_build_filename(MF_INTEGRATION_PREFIX, export_path, NULL);
    return mfd_integrator_build_host_path(integrator, export_path);
}

/**
 * mfd_integrator_build_kind_dir:
 *
 * Returns: (transfer full): the directory on the host that files of a kind are
 *   installed below
 */
static char *
mfd_integrator_build_kind_dir(MfdIntegrator *integrator, MfIntegrationKind kind)
{
    if (mfd_kind_is_in_prefix(kind))
        return g_build_filename(MF_INTEGRATION_PREFIX, mf_integration_kind_get_dir(kind), NULL);
    return mfd_integrator_build_host_path(integrator, mf_integration_kind_get_dir(kind));
}

/**
 * mfd_integrator_kind_is_supported:
 *
 * Returns: %TRUE if we install this kind of file
 */
static gboolean
mfd_integrator_kind_is_supported(MfIntegrationKind kind)
{
    switch (kind) {
    case MF_INTEGRATION_KIND_DESKTOP_ENTRY:
    case MF_INTEGRATION_KIND_ICON:
    case MF_INTEGRATION_KIND_MIME_PACKAGE:
    case MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM:
    case MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER:
    case MF_INTEGRATION_KIND_UDEV_RULE:
        return TRUE;
    default:
        return FALSE;
    }
}

/**
 * mfd_integrator_list_host_paths:
 *
 * Returns: (transfer full) (element-type utf8): the paths on the host that an
 *   integration section installs files to
 */
static GPtrArray *
mfd_integrator_list_host_paths(MfdIntegrator *integrator, MfIntegration *integration)
{
    GPtrArray *files = mf_integration_get_files(integration);
    GPtrArray *aliases = mf_integration_get_aliases(integration);
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < files->len; i++) {
        MfIntegrationFile *file = g_ptr_array_index(files, i);

        if (mfd_integrator_kind_is_supported(mf_integration_file_get_kind(file)))
            g_ptr_array_add(paths, mfd_integrator_build_file_path(integrator, file));
    }
    for (guint i = 0; i < aliases->len; i++) {
        MfIntegrationAlias *alias = g_ptr_array_index(aliases, i);

        g_ptr_array_add(
            paths,
            mfd_integrator_build_host_path(integrator, mf_integration_alias_get_export_path(alias)));
    }

    return g_steal_pointer(&paths);
}

/**
 * mfd_integrator_export_new:
 *
 * Create the export for an integration file. An icon is exported as a symbolic
 * link with a target below the `current` link of the component, so it always
 * points to the icon of the current version. All other files are copied.
 *
 * Returns: (transfer full): the export, or %NULL on error
 */
static MfdExport *
mfd_integrator_export_new(MfdIntegrator *integrator,
                          MfComponent *component,
                          MfManifest *manifest,
                          MfIntegrationFile *file,
                          GBytes *content,
                          GError **error)
{
    const char *root = mf_store_get_path(integrator->store);
    const char *export_path = mf_integration_file_get_export_path(file);
    g_autoptr(MfdExport) export = g_new0(MfdExport, 1);

    export->path = mfd_integrator_build_file_path(integrator, file);
    export->kind = mf_integration_file_get_kind(file);
    export->dir = mfd_integrator_build_kind_dir(integrator, export->kind);
    if (export->kind == MF_INTEGRATION_KIND_ICON) {
        export->target = g_build_filename(root,
                                          mf_store_subdir_for_kind(mf_component_get_kind(component), FALSE),
                                          mf_component_get_id(component),
                                          MF_STORE_CURRENT_NAME,
                                          MF_STORE_EXPORTS_NAME,
                                          export_path,
                                          NULL);
        return g_steal_pointer(&export);
    }

    export->content = mf_integration_file_render(file, manifest, content, error);
    if (export->content == NULL)
        return NULL;
    export->sha256 = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, export->content);
    export->mode = EXPORT_FILE_MODE;

    return g_steal_pointer(&export);
}

/**
 * mfd_integrator_alias_export_new:
 *
 * Create the export for an alias, which is a script that runs a program of the image.
 *
 * Returns: (transfer full): the export, or %NULL on error
 */
static MfdExport *
mfd_integrator_alias_export_new(MfdIntegrator *integrator,
                                MfManifest *manifest,
                                MfIntegrationAlias *alias,
                                GError **error)
{
    g_autoptr(MfdExport) export = g_new0(MfdExport, 1);

    g_autofree char *alias_dir = g_path_get_dirname(mf_integration_alias_get_export_path(alias));

    export->path = mfd_integrator_build_host_path(integrator, mf_integration_alias_get_export_path(alias));
    export->kind = MF_INTEGRATION_KIND_UNKNOWN;
    export->dir = mfd_integrator_build_host_path(integrator, alias_dir);
    export->content = mf_integration_alias_render(alias, manifest, error);
    if (export->content == NULL)
        return NULL;
    export->sha256 = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, export->content);
    export->mode = EXPORT_ALIAS_MODE;

    return g_steal_pointer(&export);
}

/**
 * mfd_integrator_collect_exports:
 * @component: (nullable): the current version of a component, if it has one
 * @result: (nullable): the result that gets a warning if the files of the version are not installed
 *
 * Returns: (transfer full) (element-type utf8 MfdExport): the files that have to
 *   be on the host for @component, or %NULL on error
 */
static GHashTable *
mfd_integrator_collect_exports(MfdIntegrator *integrator,
                               MfComponent *component,
                               MfResult *result,
                               GError **error)
{
    g_autoptr(GHashTable) wanted = mfd_exports_new();
    g_autoptr(GHashTable) contents = NULL;
    g_autoptr(MfManifest) manifest = NULL;
    g_autoptr(GError) rules_error = NULL;
    MfIntegration *integration;
    GPtrArray *files;
    GPtrArray *aliases;

    if (component == NULL)
        return g_steal_pointer(&wanted);
    manifest = mf_component_fetch_manifest(component, error);
    if (manifest == NULL)
        return NULL;
    integration = mf_manifest_get_integration(manifest);
    if (integration == NULL)
        return g_steal_pointer(&wanted);

    /* check the integration rules again, as they may have become stricter since this
     * version was installed */
    if (!mf_manifest_check_integration(manifest, &rules_error)) {
        mfd_emit_warning(result,
                         "Not installing the host files of %s %s: %s",
                         mf_component_get_id(component),
                         mf_component_get_full_version(component),
                         rules_error->message);
        return g_steal_pointer(&wanted);
    }

    contents = mfd_integrator_read_exports(integrator, component, manifest, error);
    if (contents == NULL)
        return NULL;

    files = mf_integration_get_files(integration);
    for (guint i = 0; i < files->len; i++) {
        MfIntegrationFile *file = g_ptr_array_index(files, i);
        GBytes *content = g_hash_table_lookup(contents, mf_integration_file_get_export_path(file));
        MfdExport *export;

        if (!mfd_integrator_kind_is_supported(mf_integration_file_get_kind(file)))
            continue;
        export = mfd_integrator_export_new(integrator, component, manifest, file, content, error);
        if (export == NULL)
            return NULL;
        mfd_exports_add(wanted, export);
    }

    aliases = mf_integration_get_aliases(integration);
    for (guint i = 0; i < aliases->len; i++) {
        MfdExport *export = mfd_integrator_alias_export_new(integrator,
                                                            manifest,
                                                            g_ptr_array_index(aliases, i),
                                                            error);

        if (export == NULL)
            return NULL;
        mfd_exports_add(wanted, export);
    }

    return g_steal_pointer(&wanted);
}

/**
 * mfd_export_copy:
 *
 * Returns: (transfer full): a copy with only the fields that a registry records
 */
static MfdExport *
mfd_export_copy(const MfdExport *export)
{
    MfdExport *copy = g_new0(MfdExport, 1);

    copy->path = g_strdup(export->path);
    copy->sha256 = g_strdup(export->sha256);
    copy->target = g_strdup(export->target);

    return copy;
}

/**
 * mfd_integrator_check_conflicts:
 * @integrator: an #MfdIntegrator
 * @manifests: (element-type MfManifest): the versions that are about to become current
 * @error: a #GError
 *
 * Check that the given versions can install their files on the host, without
 * replacing a file they do not own.
 *
 * Returns: %TRUE if there is no conflict
 */
gboolean
mfd_integrator_check_conflicts(MfdIntegrator *integrator, GPtrArray *manifests, GError **error)
{
    g_autoptr(GHashTable) owners = NULL;

    g_return_val_if_fail(MFD_IS_INTEGRATOR(integrator), FALSE);

    owners = mfd_integrator_read_owners(integrator, error);
    if (owners == NULL)
        return FALSE;

    for (guint i = 0; i < manifests->len; i++) {
        MfManifest *manifest = g_ptr_array_index(manifests, i);
        const char *component_id = mf_manifest_get_component_id(manifest);
        MfIntegration *integration = mf_manifest_get_integration(manifest);
        g_autoptr(GPtrArray) paths = NULL;

        if (integration == NULL)
            continue;

        paths = mfd_integrator_list_host_paths(integrator, integration);
        for (guint j = 0; j < paths->len; j++) {
            const char *path = g_ptr_array_index(paths, j);

            if (!mfd_owners_check_path(owners, path, component_id, error))
                return FALSE;

            /* the manifests that follow must not install a file at this path either */
            g_hash_table_replace(owners, g_strdup(path), g_strdup(component_id));
        }
    }

    return TRUE;
}

/**
 * mfd_integrator_drop_conflicts:
 * @result: (nullable): the result that gets a warning for every file that is left out
 * @component_id: the component that wants to install the files
 * @wanted: (element-type utf8 MfdExport): the files to install
 *
 * Remove the files from @wanted that would replace a file which the component
 * does not own.
 *
 * A conflict can occur when a removal makes an older version current, as the
 * files of the older version were not checked for conflicts. We keep the
 * existing file on the host and emit a warning.
 *
 * Returns: %TRUE on success
 */
static gboolean
mfd_integrator_drop_conflicts(MfdIntegrator *integrator,
                              MfResult *result,
                              const char *component_id,
                              GHashTable *wanted,
                              GError **error)
{
    g_autoptr(GHashTable) owners = mfd_integrator_read_owners(integrator, error);
    GHashTableIter iter;
    gpointer path;

    if (owners == NULL)
        return FALSE;

    g_hash_table_iter_init(&iter, wanted);
    while (g_hash_table_iter_next(&iter, &path, NULL)) {
        g_autoptr(GError) conflict = NULL;

        if (mfd_owners_check_path(owners, path, component_id, &conflict))
            continue;
        mfd_emit_warning(result, "Skipping a file: %s", conflict->message);
        g_hash_table_iter_remove(&iter);
    }

    return TRUE;
}

/**
 * mfd_host_tool_child_setup:
 *
 * Set the timeout of a host tool, between fork and exec. The alarm timer stays
 * set across the exec, and kills the tool if it does not finish in time.
 */
static void
mfd_host_tool_child_setup(gpointer user_data)
{
    alarm(HOST_TOOL_TIMEOUT_SEC);
}

/**
 * mfd_host_tool_run:
 * @argv: the name of the tool, followed by its arguments
 * @result: (nullable): the result that gets a warning if the tool fails
 *
 * Run a tool of the host if it is installed, and only warn if it fails.
 */
static void
mfd_host_tool_run(const char *const *argv, MfResult *result)
{
    g_autofree char *tool = g_find_program_in_path(argv[0]);
    g_autofree char *output = NULL;
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GError) error = NULL;

    if (tool == NULL) {
        g_debug("Not running %s, as it is not installed", argv[0]);
        return;
    }

    /* capture the output, we show it if the tool fails */
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_set_child_setup(launcher, mfd_host_tool_child_setup, NULL, NULL);
    proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
    if (proc == NULL || !g_subprocess_communicate_utf8(proc, NULL, NULL, &output, NULL, &error)) {
        mfd_emit_warning(result, "Unable to run %s: %s", argv[0], error->message);
        return;
    }
    if (!g_subprocess_get_successful(proc))
        mfd_emit_warning(result, "%s failed: %s", argv[0], g_strstrip(output));
}

/**
 * mfd_path_get_name_of_kind:
 * @path: the path of a file on the host
 * @kind: a kind of file that we install below our prefix
 *
 * Returns: (nullable): the name of the file as a pointer into @path, or %NULL
 *   if the file is not of this kind
 */
static const char *
mfd_path_get_name_of_kind(const char *path, MfIntegrationKind kind)
{
    g_autofree char *dir = g_strconcat(MF_INTEGRATION_PREFIX "/",
                                       mf_integration_kind_get_dir(kind),
                                       "/",
                                       NULL);

    return g_str_has_prefix(path, dir) ? path + strlen(dir) : NULL;
}

/**
 * mfd_exports_have_kind:
 * @exports: (element-type utf8 MfdExport): a table of files
 * @kind: a kind of file that we install below our prefix
 *
 * Returns: %TRUE if a file of this kind is among @exports
 */
static gboolean
mfd_exports_have_kind(GHashTable *exports, MfIntegrationKind kind)
{
    GHashTableIter iter;
    gpointer path;

    g_hash_table_iter_init(&iter, exports);
    while (g_hash_table_iter_next(&iter, &path, NULL)) {
        if (mfd_path_get_name_of_kind(path, kind) != NULL)
            return TRUE;
    }

    return FALSE;
}

/**
 * mfd_unit_build_pattern:
 * @unit: the name of a unit file
 *
 * Returns: (transfer full): a pattern that makes systemctl act on the running
 *   units of a unit file. For a template, the pattern matches all of its instances.
 */
static char *
mfd_unit_build_pattern(const char *unit)
{
    const char *at = strchr(unit, '@');

    if (at == NULL)
        return g_strdup(unit);
    return g_strdup_printf("%.*s@*%s", (int) (at - unit), unit, at + 1);
}

/**
 * mfd_systemctl_run:
 * @command: the command of systemctl and its options, %NULL terminated
 * @units: (element-type utf8): the units to act on
 * @result: (nullable): the result that gets a warning if systemctl fails
 *
 * Run a systemctl command for a list of units, if the list is not empty.
 *
 * We do not wait for the units to change their state. A service can take longer
 * to stop or restart than the timeout for host tools allows, and we can not
 * handle other requests while we wait. Systemd completes the job even if the
 * unit file was removed in the meantime.
 */
static void
mfd_systemctl_run(const char *const *command, GPtrArray *units, MfResult *result)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new();

    if (units->len == 0)
        return;

    g_ptr_array_add(argv, "systemctl");
    for (guint i = 0; command[i] != NULL; i++)
        g_ptr_array_add(argv, (char *) command[i]);
    g_ptr_array_add(argv, "--no-block");
    for (guint i = 0; i < units->len; i++)
        g_ptr_array_add(argv, g_ptr_array_index(units, i));
    g_ptr_array_add(argv, NULL);

    mfd_host_tool_run((const char *const *) argv->pdata, result);
}

/**
 * mfd_restore_selinux_labels:
 * @result: (nullable): the result that gets a warning if restorecon fails
 *
 * Set the SELinux labels of the files below our prefix to the labels that the
 * policy of the host defines. A file that we create inherits the label of its
 * directory, which is the wrong label if we had to create the directory too.
 */
static void
mfd_restore_selinux_labels(MfResult *result)
{
    g_autofree char *units_dir = g_build_filename(MF_INTEGRATION_PREFIX, "lib", "systemd", NULL);
    g_autofree char *rules_dir = g_build_filename(MF_INTEGRATION_PREFIX, "lib", "udev", NULL);
    const char *const argv[] = {"restorecon", "-R", units_dir, rules_dir, NULL};

    if (g_file_test("/sys/fs/selinux/enforce", G_FILE_TEST_EXISTS))
        mfd_host_tool_run(argv, result);
}

/**
 * mfd_integrator_update_host_caches:
 * @result: (nullable): the result that gets a warning if a tool fails
 *
 * Update the caches of the host that are built from our desktop entries and
 * MIME packages. Icons need no cache update, as our exports directory never
 * has an icon cache file.
 *
 * update-desktop-database and update-mime-database fail on a directory that
 * does not exist, which is the case until the first application has installed
 * a file of that kind.
 */
static void
mfd_integrator_update_host_caches(MfdIntegrator *integrator, MfResult *result)
{
    const char *root = mf_store_get_path(integrator->store);
    g_autofree char *applications = g_build_filename(
        root,
        MF_STORE_EXPORTS_NAME,
        mf_integration_kind_get_dir(MF_INTEGRATION_KIND_DESKTOP_ENTRY),
        NULL);
    g_autofree char *packages = g_build_filename(
        root,
        MF_STORE_EXPORTS_NAME,
        mf_integration_kind_get_dir(MF_INTEGRATION_KIND_MIME_PACKAGE),
        NULL);
    g_autofree char *mime = g_path_get_dirname(packages);

    if (mfd_is_directory(applications)) {
        const char *const argv[] = {"update-desktop-database", "--quiet", applications, NULL};

        mfd_host_tool_run(argv, result);
    }
    if (mfd_is_directory(packages)) {
        const char *const argv[] = {"update-mime-database", mime, NULL};

        mfd_host_tool_run(argv, result);
    }
}

/**
 * mfd_integrator_reconcile:
 * @integrator: an #MfdIntegrator
 * @result: (nullable): the result to add warnings to
 * @component_id: a component, which may not be installed anymore
 * @error: a #GError
 *
 * Update the exports of a component, so they match its current version. All
 * files that the component owns are replaced. Nothing is done if the files
 * already belong to the current version. If the update fails, the registry
 * records that the files are not up to date, and the next call tries again.
 *
 * The services of a removed unit are stopped, and the services of a kept unit
 * are restarted. The systemd user managers only find new user units when the
 * next session of a user starts. The udev rules are reloaded, but we do not
 * trigger events for existing devices, as these devices may be in use.
 *
 * Returns: %TRUE on success
 */
gboolean
mfd_integrator_reconcile(MfdIntegrator *integrator,
                         MfResult *result,
                         const char *component_id,
                         GError **error)
{
    g_autoptr(MfdRegistry) registry = NULL;
    g_autoptr(MfComponent) current = NULL;
    g_autoptr(GHashTable) wanted = NULL;
    g_autoptr(GPtrArray) stale = g_ptr_array_new();
    g_autoptr(GPtrArray) stale_units = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) kept_units = g_ptr_array_new_with_free_func(g_free);
    const char *const disable_command[] = {"disable", "--now", NULL};
    const char *const restart_command[] = {"try-restart", NULL};
    const char *const reload_units_argv[] = {"systemctl", "daemon-reload", NULL};
    const char *const reload_rules_argv[] = {"udevadm", "control", "--reload", NULL};
    gboolean units_changed;
    gboolean rules_changed;
    gboolean has_registry;
    GHashTableIter iter;
    gpointer value;

    g_return_val_if_fail(MFD_IS_INTEGRATOR(integrator), FALSE);

    registry = mfd_integrator_read_registry(integrator, component_id, error);
    if (registry == NULL)
        return FALSE;
    if (!mfd_integrator_find_current(integrator, component_id, &current, error))
        return FALSE;

    /* the files on the host already belong to the current version */
    if (current != NULL && g_strcmp0(registry->version, mf_component_get_full_version(current)) == 0)
        return TRUE;

    wanted = mfd_integrator_collect_exports(integrator, current, result, error);
    if (wanted == NULL)
        return FALSE;
    has_registry = g_file_test(registry->path, G_FILE_TEST_EXISTS);
    if (!has_registry && g_hash_table_size(wanted) == 0)
        return TRUE;

    if (!mfd_integrator_drop_conflicts(integrator, result, component_id, wanted, error))
        return FALSE;

    /* add the new files to the registry first */
    g_clear_pointer(&registry->version, g_free);
    g_hash_table_iter_init(&iter, registry->exports);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        MfdExport *export = value;
        const char *unit = mfd_path_get_name_of_kind(export->path, MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM);

        if (g_hash_table_contains(wanted, export->path)) {
            if (unit != NULL)
                g_ptr_array_add(kept_units, mfd_unit_build_pattern(unit));
            continue;
        }
        g_ptr_array_add(stale, export);
        if (unit != NULL)
            g_ptr_array_add(stale_units, g_strdup(unit));
    }
    g_hash_table_iter_init(&iter, wanted);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        mfd_exports_add(registry->exports, mfd_export_copy(value));
    if (!mfd_registry_save(registry, error))
        return FALSE;

    /* the registry now lists the files of both versions, some of which we are about to remove */
    units_changed = mfd_exports_have_kind(registry->exports, MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM);
    rules_changed = mfd_exports_have_kind(registry->exports, MF_INTEGRATION_KIND_UDEV_RULE);

    /* systemctl reads the [Install] section of a unit file to disable the unit, so the file
     * has to exist at this point */
    mfd_systemctl_run(disable_command, stale_units, result);

    g_hash_table_iter_init(&iter, wanted);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!mfd_export_install(value, error))
            return FALSE;
    }
    for (guint i = 0; i < stale->len; i++) {
        MfdExport *export = g_ptr_array_index(stale, i);

        if (!mfd_unlink_if_exists(export->path, error))
            return FALSE;
        g_hash_table_remove(registry->exports, export->path);
    }

    mfd_restore_selinux_labels(result);
    if (units_changed)
        mfd_host_tool_run(reload_units_argv, result);
    mfd_systemctl_run(restart_command, kept_units, result);
    if (rules_changed)
        mfd_host_tool_run(reload_rules_argv, result);
    mfd_integrator_update_host_caches(integrator, result);

    if (current == NULL || g_hash_table_size(wanted) == 0)
        return mfd_unlink_if_exists(registry->path, error);

    registry->version = g_strdup(mf_component_get_full_version(current));
    return mfd_registry_save(registry, error);
}

/**
 * mfd_integrator_reconcile_all:
 * @integrator: an #MfdIntegrator
 *
 * Update the exports of all components, to complete an update that an earlier
 * operation did not finish, for example because we crashed.
 *
 * We check the installed applications in addition to the components that have
 * a registry, as an application has no registry yet if we crashed right after
 * its first installation. Failures are only logged.
 */
void
mfd_integrator_reconcile_all(MfdIntegrator *integrator)
{
    g_autoptr(GHashTable) ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GPtrArray) apps = NULL;
    g_autoptr(GError) error = NULL;
    GHashTableIter iter;
    gpointer key;

    g_return_if_fail(MFD_IS_INTEGRATOR(integrator));

    apps = mf_store_list_components(integrator->store, NULL, MF_COMPONENT_KIND_APP, &error);
    if (apps == NULL) {
        g_warning("Unable to list the installed applications: %s", error->message);
        return;
    }
    for (guint i = 0; i < apps->len; i++)
        g_hash_table_add(ids, g_strdup(mf_component_get_id(g_ptr_array_index(apps, i))));
    mfd_integrator_add_registered_ids(integrator, ids);

    g_hash_table_iter_init(&iter, ids);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        g_autoptr(GError) reconcile_error = NULL;

        if (!mfd_integrator_reconcile(integrator, NULL, key, &reconcile_error))
            g_warning("Unable to update the host files of %s: %s",
                      (const char *) key,
                      reconcile_error->message);
    }
}
