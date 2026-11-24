/*
 * Copyright (C) 2020-2026 Collabora Ltd.
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
#include "config.h"
#include "mfr-capture.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libglnx.h>
#include <systemd/sd-json.h>

#include "mf-sdautoptr-private.h"
#include "mf-json-private.h"
#include "mf-utils-private.h"
#include "mfr-capture-build.h"
#include "mfr-tree.h"

/*
 * The graphics stack of the host, captured for a Manifold core.
 *
 * mfr_capture_build() collects the libraries, drivers and loader manifests
 * that have to be used from the host, see mfr-capture-build.c. This file
 * manages the directory that the result is stored in.
 *
 * A capture is the directory MF_RUN_CACHE_DIR/capture/<core-hash>/ with
 * these entries:
 *
 *   overrides/          the overlay layer that is placed on top of the images
 *   environment.json    the environment variables for the loaders. A variable
 *                       with the value "" must be unset.
 *   stamp               the files and directories of the host that the
 *                       capture was made from, with their modification times
 *
 * An #MfrCapture describes the capture for one core image.
 * mfr_capture_ensure() loads it from its directory, or builds it. A capture
 * is stale if a host library that it links to has changed, or if a loader
 * manifest was added, removed or replaced. We check every link when the
 * daemon starts. Before an app launch, we only compare the modification
 * times of the library and manifest directories.
 *
 * While the daemon runs, a capture that we do not use anymore is only moved
 * aside. It is deleted the next time the daemon starts.
 */

constexpr char OVERRIDES_NAME[] = "overrides";
constexpr char ENVIRONMENT_NAME[] = "environment.json";
constexpr char STAMP_NAME[] = "stamp";

constexpr char STALE_PREFIX[] = ".stale-";

struct _MfrCapture {
    GObject parent_instance;

    char *dir;
    char *overrides_dir;
    char *arch_tuple;   /* NULL for a capture that was created for an existing directory */
    char *id;           /* unique ID of the capture, NULL until the capture is taken */
    char **environment; /* VAR=VALUE entries, an empty VALUE means the variable must be unset */
    sd_json_variant *stamp;
};

G_DEFINE_FINAL_TYPE(MfrCapture, mfr_capture, G_TYPE_OBJECT)

/**
 * mfr_capture_init:
 */
static void
mfr_capture_init(MfrCapture *capture)
{
}

/**
 * mfr_capture_finalize:
 */
static void
mfr_capture_finalize(GObject *object)
{
    MfrCapture *capture = MFR_CAPTURE(object);

    g_free(capture->dir);
    g_free(capture->overrides_dir);
    g_free(capture->arch_tuple);
    g_free(capture->id);
    g_strfreev(capture->environment);
    sd_json_variant_unref(capture->stamp);

    G_OBJECT_CLASS(mfr_capture_parent_class)->finalize(object);
}

/**
 * mfr_capture_class_init:
 */
static void
mfr_capture_class_init(MfrCaptureClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mfr_capture_finalize;
}

/**
 * mfr_stat_mtime_ns:
 *
 * Returns: the modification time of @st, in nanoseconds
 */
static uint64_t
mfr_stat_mtime_ns(const struct stat *st)
{
    return (uint64_t) st->st_mtim.tv_sec * G_USEC_PER_SEC * 1000 + (uint64_t) st->st_mtim.tv_nsec;
}

/**
 * mfr_stamp_add_dir:
 *
 * Add a host directory and its modification time to the directory entries
 * of a stamp. A missing directory is recorded with the modification time 0.
 *
 * Returns: 0, or a negative errno
 */
static int
mfr_stamp_add_dir(sd_json_variant **dirs, const char *path)
{
    struct stat st;
    uint64_t mtime = stat(path, &st) == 0 ? mfr_stat_mtime_ns(&st) : 0;

    return sd_json_variant_append_arraybo(dirs,
                                          SD_JSON_BUILD_PAIR_STRING("path", path),
                                          SD_JSON_BUILD_PAIR_UNSIGNED("mtime", mtime));
}

/**
 * MfrStampTargets:
 *
 * The host files that the links of a capture point to, for the stamp.
 * Every file is only recorded once.
 */
typedef struct {
    sd_json_variant *entries; /* array with the path, size and modification time of every file */
    GHashTable *paths;        /* set of the paths of the recorded files */
} MfrStampTargets;

/**
 * mfr_stamp_targets_clear:
 */
static void
mfr_stamp_targets_clear(MfrStampTargets *targets)
{
    g_clear_pointer(&targets->entries, sd_json_variant_unref);
    g_clear_pointer(&targets->paths, g_hash_table_unref);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfrStampTargets, mfr_stamp_targets_clear)

/**
 * mfr_stamp_targets_add:
 *
 * Add a host file to @targets, with its size and modification time,
 * unless the file was added before.
 */
static gboolean
mfr_stamp_targets_add(MfrStampTargets *targets, const char *host_path, GError **error)
{
    struct stat st;
    int r;

    if (!g_hash_table_add(targets->paths, g_strdup(host_path)))
        return TRUE;
    if (stat(host_path, &st) != 0)
        return glnx_throw_errno_prefix(error, "Captured %s is missing on the host", host_path);
    r = sd_json_variant_append_arraybo(&targets->entries,
                                       SD_JSON_BUILD_PAIR_STRING("path", host_path),
                                       SD_JSON_BUILD_PAIR_UNSIGNED("size", (uint64_t) st.st_size),
                                       SD_JSON_BUILD_PAIR_UNSIGNED("mtime", mfr_stat_mtime_ns(&st)));
    if (r < 0)
        return glnx_throw(error, "Unable to record %s: %s", host_path, g_strerror(-r));

    return TRUE;
}

/**
 * mfr_stamp_add_link_targets:
 *
 * Search @dir recursively for links to host files, and add every host file
 * that a link points to to @targets.
 */
static gboolean
mfr_stamp_add_link_targets(const char *dir, MfrStampTargets *targets, GError **error)
{
    g_auto(GLnxDirFdIterator) iter = {0};
    struct dirent *entry;

    if (!glnx_dirfd_iterator_init_at(AT_FDCWD, dir, FALSE, &iter, error))
        return FALSE;
    while (TRUE) {
        g_autofree char *path = NULL;

        if (!glnx_dirfd_iterator_next_dent_ensure_dtype(&iter, &entry, NULL, error))
            return FALSE;
        if (entry == NULL)
            break;

        path = g_build_filename(dir, entry->d_name, NULL);
        if (entry->d_type == DT_DIR) {
            if (!mfr_stamp_add_link_targets(path, targets, error))
                return FALSE;
        } else if (entry->d_type == DT_LNK) {
            g_autofree char *target = g_file_read_link(path, NULL);

            if (target == NULL || !g_str_has_prefix(target, MFR_HOST_DIR))
                continue;
            if (!mfr_stamp_targets_add(targets, target + strlen(MFR_HOST_DIR), error))
                return FALSE;
        }
    }

    return TRUE;
}

/**
 * mfr_capture_write_stamp:
 * @host_dirs: (element-type utf8): the directories of the host that the capture depends on
 *
 * Write the stamp of the capture in @dir. The stamp records the directories
 * and files of the host that the capture depends on.
 */
static gboolean
mfr_capture_write_stamp(const char *dir, const char *arch_tuple, GPtrArray *host_dirs, GError **error)
{
    g_autoptr(sd_json_variant) stamp = NULL;
    g_autoptr(sd_json_variant) dirs = NULL;
    g_auto(MfrStampTargets) targets = {
        .paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
    };
    g_autofree char *overrides_dir = g_build_filename(dir, OVERRIDES_NAME, NULL);
    g_autofree char *id = g_uuid_string_random();
    g_autofree char *path = g_build_filename(dir, STAMP_NAME, NULL);
    int r;

    /* the mtime of a directory changes when a file is added to it, removed or replaced */
    for (guint i = 0; i < host_dirs->len; i++) {
        if (mfr_stamp_add_dir(&dirs, g_ptr_array_index(host_dirs, i)) < 0)
            return glnx_throw(error, "Unable to record directories");
    }
    if (!mfr_stamp_add_link_targets(overrides_dir, &targets, error))
        return FALSE;
    if (targets.entries == NULL)
        r = sd_json_variant_new_array(&targets.entries, NULL, 0);
    else
        r = 0;
    if (r >= 0)
        r = sd_json_buildo(&stamp,
                           SD_JSON_BUILD_PAIR_STRING("id", id),
                           SD_JSON_BUILD_PAIR_STRING("tuple", arch_tuple),
                           SD_JSON_BUILD_PAIR_VARIANT("dirs", dirs),
                           SD_JSON_BUILD_PAIR_VARIANT("targets", targets.entries));
    if (r < 0)
        return glnx_throw(error, "Unable to build stamp: %s", g_strerror(-r));

    return mf_json_write_file(path, stamp, error);
}

/**
 * mfr_capture_write_environment:
 * @environment: the environment variables for the loaders, as VAR=VALUE entries
 *
 * Write the environment file of the capture in @dir.
 */
static gboolean
mfr_capture_write_environment(const char *dir, const char *const *environment, GError **error)
{
    g_autoptr(sd_json_variant) object = NULL;
    g_autofree char *path = g_build_filename(dir, ENVIRONMENT_NAME, NULL);

    for (size_t i = 0; environment[i] != NULL; i++) {
        const char *separator = strchr(environment[i], '=');
        g_autofree char *name = NULL;
        int r;

        g_return_val_if_fail(separator != NULL, FALSE);
        name = g_strndup(environment[i], (size_t) (separator - environment[i]));
        r = sd_json_variant_set_field_string(&object, name, separator + 1);
        if (r < 0)
            return glnx_throw(error, "Unable to build environment: %s", g_strerror(-r));
    }

    return mf_json_write_file(path, object, error);
}

/**
 * mfr_capture_load_environment:
 *
 * Returns: (transfer full): the environment of the capture in @dir, as VAR=VALUE entries
 */
static char **
mfr_capture_load_environment(const char *dir, GError **error)
{
    g_autofree char *path = g_build_filename(dir, ENVIRONMENT_NAME, NULL);
    g_autoptr(sd_json_variant) environment = mf_json_parse_file(path, error);
    g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_free);

    if (environment == NULL)
        return NULL;
    if (!sd_json_variant_is_object(environment))
        return glnx_null_throw(error, "%s is not an object", path);

    for (size_t i = 0; i + 1 < sd_json_variant_elements(environment); i += 2) {
        sd_json_variant *key = sd_json_variant_by_index(environment, i);
        sd_json_variant *value = sd_json_variant_by_index(environment, i + 1);

        if (!sd_json_variant_is_string(key) || !sd_json_variant_is_string(value))
            return glnx_null_throw(error, "%s does not map names to strings", path);
        g_ptr_array_add(entries,
                        g_strdup_printf("%s=%s", sd_json_variant_string(key), sd_json_variant_string(value)));
    }
    g_ptr_array_add(entries, NULL);

    return (char **) g_ptr_array_free(g_steal_pointer(&entries), FALSE);
}

/**
 * mfr_capture_new_for_dir:
 *
 * Create an #MfrCapture for the directory @dir, without loading the capture.
 * We use this function when scanning the capture directory, which may also
 * contain captures that were moved aside or are incomplete.
 */
static MfrCapture *
mfr_capture_new_for_dir(const char *dir)
{
    MfrCapture *capture = g_object_new(MFR_TYPE_CAPTURE, NULL);

    capture->dir = g_strdup(dir);
    capture->overrides_dir = g_build_filename(dir, OVERRIDES_NAME, NULL);
    return capture;
}

/**
 * mfr_capture_new:
 * @core_hash: the verity hash of the core that the capture is for
 * @arch_tuple: the multiarch tuple of the applications that run on the core
 *
 * Create an #MfrCapture for a core. The capture is not loaded or built
 * until mfr_capture_ensure() is called.
 *
 * Returns: (transfer full): the capture
 */
MfrCapture *
mfr_capture_new(const char *core_hash, const char *arch_tuple)
{
    g_autofree char *dir = g_build_filename(MFR_CAPTURE_DIR, core_hash, NULL);
    MfrCapture *capture = mfr_capture_new_for_dir(dir);

    capture->arch_tuple = g_strdup(arch_tuple);
    return capture;
}

/**
 * mfr_capture_unload:
 *
 * Clear the ID, environment and stamp that were loaded from the directory of the capture.
 */
static void
mfr_capture_unload(MfrCapture *capture)
{
    g_clear_pointer(&capture->id, g_free);
    g_clear_pointer(&capture->environment, g_strfreev);
    g_clear_pointer(&capture->stamp, sd_json_variant_unref);
}

/**
 * mfr_capture_load:
 *
 * Load the stamp and the environment from the directory of the capture.
 * They replace the previously loaded stamp and environment.
 *
 * Returns: %FALSE if the capture in the directory is incomplete. The
 *   previously loaded data is kept in that case.
 */
static gboolean
mfr_capture_load(MfrCapture *capture, GError **error)
{
    g_autofree char *stamp_path = g_build_filename(capture->dir, STAMP_NAME, NULL);
    g_autoptr(sd_json_variant) stamp = mf_json_parse_file(stamp_path, error);
    g_auto(GStrv) environment = NULL;
    const char *id;

    if (stamp == NULL)
        return FALSE;
    id = mf_json_get_string(stamp, "id");
    if (id == NULL || !sd_json_variant_is_array(sd_json_variant_by_key(stamp, "dirs")) ||
        !sd_json_variant_is_array(sd_json_variant_by_key(stamp, "targets")))
        return glnx_throw(error, "%s is not a capture stamp", stamp_path);
    environment = mfr_capture_load_environment(capture->dir, error);
    if (environment == NULL)
        return FALSE;

    mfr_capture_unload(capture);
    capture->id = g_strdup(id);
    capture->environment = g_steal_pointer(&environment);
    capture->stamp = g_steal_pointer(&stamp);
    return TRUE;
}

/**
 * mfr_capture_new_from_dir:
 *
 * Load the capture in @dir.
 *
 * Returns: (transfer full): the capture, or %NULL if it is incomplete
 */
static MfrCapture *
mfr_capture_new_from_dir(const char *dir, GError **error)
{
    g_autoptr(MfrCapture) capture = mfr_capture_new_for_dir(dir);

    if (!mfr_capture_load(capture, error))
        return NULL;
    return g_steal_pointer(&capture);
}

/**
 * mfr_stamp_entry_changed:
 *
 * Check whether the file or directory of a stamp entry has changed since
 * the capture was made.
 */
static gboolean
mfr_stamp_entry_changed(sd_json_variant *entry)
{
    sd_json_variant *path = sd_json_variant_by_key(entry, "path");
    sd_json_variant *mtime = sd_json_variant_by_key(entry, "mtime");
    sd_json_variant *size = sd_json_variant_by_key(entry, "size");
    struct stat st;

    if (!sd_json_variant_is_string(path) || !sd_json_variant_is_unsigned(mtime))
        return TRUE;
    if (stat(sd_json_variant_string(path), &st) != 0)
        return sd_json_variant_unsigned(mtime) != 0;
    if (mfr_stat_mtime_ns(&st) != sd_json_variant_unsigned(mtime))
        return TRUE;
    if (size != NULL &&
        (!sd_json_variant_is_unsigned(size) || sd_json_variant_unsigned(size) != (uint64_t) st.st_size))
        return TRUE;
    return FALSE;
}

/**
 * mfr_stamp_find_changed:
 *
 * Find the first entry in the list @key of a stamp whose file or directory
 * has changed.
 *
 * Returns: %TRUE if an entry has changed, with its path stored in @ret_changed
 */
static gboolean
mfr_stamp_find_changed(sd_json_variant *stamp, const char *key, const char **ret_changed)
{
    sd_json_variant *entries = sd_json_variant_by_key(stamp, key);

    for (size_t i = 0; i < sd_json_variant_elements(entries); i++) {
        sd_json_variant *entry = sd_json_variant_by_index(entries, i);

        if (mfr_stamp_entry_changed(entry)) {
            sd_json_variant *path = sd_json_variant_by_key(entry, "path");

            *ret_changed = sd_json_variant_is_string(path) ? sd_json_variant_string(path) : "?";
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * mfr_capture_is_fresh:
 *
 * Check whether the library and manifest directories of the host are
 * unchanged since the capture was made. We only compare the modification
 * times of the directories, which is cheap enough to do for every launch.
 */
gboolean
mfr_capture_is_fresh(const MfrCapture *capture)
{
    const char *changed;

    if (mfr_stamp_find_changed(capture->stamp, "dirs", &changed)) {
        g_info("Capture %s is stale: %s changed", capture->dir, changed);
        return FALSE;
    }
    return TRUE;
}

/**
 * mfr_capture_move_aside:
 * @reason: why we do not use the capture anymore, for the log
 *
 * Rename the directory of a capture that we do not use anymore, so a new
 * capture for the same core can be built.
 *
 * We never delete a capture while the daemon runs. A tree that was
 * assembled with the capture may have been sent to a runner that has not
 * attached the tree yet, and such a tree is not listed in any mount table.
 * Mounted overlays are not affected by the rename, because they refer to
 * the directory itself and not to its path. The daemon deletes the unused
 * captures the next time it starts.
 */
static gboolean
mfr_capture_move_aside(const MfrCapture *capture, const char *reason, GError **error)
{
    g_autofree char *name = g_path_get_basename(capture->dir);
    g_autofree char *stale_dir = NULL;

    if (g_str_has_prefix(name, STALE_PREFIX))
        return TRUE;

    stale_dir = g_strconcat(MFR_CAPTURE_DIR "/", STALE_PREFIX, capture->id, NULL);
    g_info("Moving capture %s aside: %s", name, reason);
    if (rename(capture->dir, stale_dir) != 0)
        return glnx_throw_errno_prefix(error, "Unable to rename %s to %s", capture->dir, stale_dir);
    return TRUE;
}

/**
 * mfr_capture_retire:
 * @used_ids: (nullable): the IDs of the captures of all trees that are still in use
 * @reason: why we do not use the capture anymore, for the log
 *
 * Delete a capture that we do not use anymore. If a tree that is still in
 * use was assembled with the capture, we only move the capture aside.
 *
 * This function must only be called when the daemon starts, because at that
 * time no tree can be in transit to a runner.
 */
static gboolean
mfr_capture_retire(const MfrCapture *capture, GHashTable *used_ids, const char *reason, GError **error)
{
    g_autofree char *name = g_path_get_basename(capture->dir);

    if (used_ids != NULL && g_hash_table_contains(used_ids, capture->id))
        return mfr_capture_move_aside(capture, reason, error);

    g_info("Removing capture %s: %s", name, reason);
    return glnx_shutil_rm_rf_at(AT_FDCWD, capture->dir, NULL, error);
}

/**
 * mfr_capture_ensure:
 * @core_mount: the path at which the core of the capture is mounted
 *
 * Make sure that the capture exists and is up to date with the host.
 *
 * We load the capture from its directory if it is not loaded yet. If the
 * capture does not exist, or if the host has changed since the capture was
 * made, we build a new capture. A new capture gets a new ID.
 *
 * Returns: %TRUE if the capture can be used
 */
gboolean
mfr_capture_ensure(MfrCapture *capture, const char *core_mount, GError **error)
{
    g_autofree char *name = NULL;
    g_autofree char *tmp_dir = NULL;
    g_autofree char *tmp_overrides_dir = NULL;
    g_auto(GStrv) environment = NULL;
    g_autoptr(GPtrArray) host_dirs = NULL;
    g_autoptr(GError) load_error = NULL;

    g_return_val_if_fail(capture->arch_tuple != NULL, FALSE);

    name = g_path_get_basename(capture->dir);
    tmp_dir = g_strconcat(MFR_CAPTURE_DIR "/", MF_TMP_PREFIX, name, NULL);

    if (capture->id == NULL && g_file_test(capture->dir, G_FILE_TEST_IS_DIR) &&
        !mfr_capture_load(capture, &load_error)) {
        g_warning("Discarding capture %s: %s", capture->dir, load_error->message);
        if (!glnx_shutil_rm_rf_at(AT_FDCWD, capture->dir, NULL, error))
            return FALSE;
    }
    if (capture->id != NULL) {
        if (mfr_capture_is_fresh(capture))
            return TRUE;
        if (!mfr_capture_move_aside(capture, "the host changed", error))
            return FALSE;
        mfr_capture_unload(capture);
    }

    /* we build in a temporary directory, and only rename it to the final name once the capture is complete */
    if (!glnx_shutil_rm_rf_at(AT_FDCWD, tmp_dir, NULL, error) ||
        !glnx_shutil_mkdir_p_at(AT_FDCWD, tmp_dir, 0755, NULL, error))
        return FALSE;
    tmp_overrides_dir = g_build_filename(tmp_dir, OVERRIDES_NAME, NULL);
    if (!mfr_capture_build(tmp_overrides_dir,
                           core_mount,
                           capture->arch_tuple,
                           &environment,
                           &host_dirs,
                           error) ||
        !mfr_capture_write_environment(tmp_dir, (const char *const *) environment, error) ||
        !mfr_capture_write_stamp(tmp_dir, capture->arch_tuple, host_dirs, error)) {
        glnx_shutil_rm_rf_at(AT_FDCWD, tmp_dir, NULL, NULL);
        return FALSE;
    }
    if (rename(tmp_dir, capture->dir) != 0) {
        glnx_throw_errno_prefix(error, "Unable to rename %s to %s", tmp_dir, capture->dir);
        glnx_shutil_rm_rf_at(AT_FDCWD, tmp_dir, NULL, NULL);
        return FALSE;
    }

    return mfr_capture_load(capture, error);
}

/**
 * mfr_capture_check_all:
 * @used_ids: (nullable): the IDs of the captures of all trees that are still in use
 *
 * Check all captures, and retire every capture that is outdated because its
 * host files have changed, and every capture that was moved aside. We also
 * remove unreadable captures and the temporary directories of interrupted
 * builds. This function must only be called when the daemon starts.
 */
void
mfr_capture_check_all(GHashTable *used_ids)
{
    g_autoptr(GDir) entries = g_dir_open(MFR_CAPTURE_DIR, 0, NULL);
    const char *name;

    while (entries != NULL && (name = g_dir_read_name(entries)) != NULL) {
        g_autofree char *dir = g_build_filename(MFR_CAPTURE_DIR, name, NULL);
        g_autofree char *reason = NULL;
        g_autoptr(MfrCapture) capture = NULL;
        g_autoptr(GError) error = NULL;
        const char *changed = NULL;

        if (g_str_has_prefix(name, MF_TMP_PREFIX)) {
            reason = g_strdup("it was not completed");
        } else {
            capture = mfr_capture_new_from_dir(dir, &error);
            if (capture == NULL)
                reason = g_strdup(error->message);
            else if (g_str_has_prefix(name, STALE_PREFIX))
                reason = g_strdup("it was replaced");
            else if (mfr_stamp_find_changed(capture->stamp, "dirs", &changed) ||
                     mfr_stamp_find_changed(capture->stamp, "targets", &changed))
                reason = g_strdup_printf("the host changed: %s", changed);
        }
        if (reason == NULL)
            continue;

        g_clear_error(&error);
        if (capture != NULL) {
            if (!mfr_capture_retire(capture, used_ids, reason, &error))
                g_warning("%s", error->message);
            continue;
        }
        g_info("Removing capture %s: %s", name, reason);
        if (!glnx_shutil_rm_rf_at(AT_FDCWD, dir, NULL, &error))
            g_warning("%s", error->message);
    }
}

/**
 * mfr_capture_discard_all:
 *
 * Move every capture aside, so the next launch builds a new capture.
 */
gboolean
mfr_capture_discard_all(GError **error)
{
    g_autoptr(GDir) entries = g_dir_open(MFR_CAPTURE_DIR, 0, NULL);
    const char *name;

    while (entries != NULL && (name = g_dir_read_name(entries)) != NULL) {
        g_autofree char *dir = g_build_filename(MFR_CAPTURE_DIR, name, NULL);
        g_autoptr(MfrCapture) capture = NULL;

        if (!g_str_has_prefix(name, MF_TMP_PREFIX))
            capture = mfr_capture_new_from_dir(dir, NULL);
        if (capture != NULL) {
            if (!mfr_capture_move_aside(capture, "a fresh capture was requested", error))
                return FALSE;
            continue;
        }
        g_info("Removing capture %s", name);
        if (!glnx_shutil_rm_rf_at(AT_FDCWD, dir, NULL, error))
            return FALSE;
    }
    return TRUE;
}

const char *
mfr_capture_get_id(const MfrCapture *capture)
{
    return capture->id;
}

const char *
mfr_capture_get_overrides_dir(const MfrCapture *capture)
{
    return capture->overrides_dir;
}

const char *const *
mfr_capture_get_environment(const MfrCapture *capture)
{
    return (const char *const *) capture->environment;
}
