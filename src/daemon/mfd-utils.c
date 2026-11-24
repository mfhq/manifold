/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libglnx.h>

/**
 * mfd_emit_warning:
 * @result: (nullable): the result of the operation that the warning belongs to
 * @format: a printf() format for the message
 *
 * Log a problem that does not fail an operation. The message is also added
 * to @result, so the client that started the operation receives it.
 */
void
mfd_emit_warning(MfResult *result, const char *format, ...)
{
    g_autofree char *message = NULL;
    va_list args;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);

    g_warning("%s", message);
    if (result != NULL)
        mf_result_add_warning(result, message);
}

/**
 * mfd_component_matches_version:
 * @version: a version, or a full version if @full is set
 *
 * Check whether @version matches the version of an installed component.
 */
gboolean
mfd_component_matches_version(MfComponent *component, const char *version, gboolean full)
{
    return g_str_equal(full ? mf_component_get_full_version(component) : mf_component_get_version(component),
                       version);
}

/**
 * mfd_store_check_version_is_full:
 * @version: (nullable): a version, which selects all of its revisions, or
 *   a full version (`<version>-<rev>`), which selects one revision
 * @ret_full: (out): whether @version is a full version
 *
 * Find out whether @version is a version or a full version, by comparing it
 * with the installed versions of the component.
 *
 * A version may itself end in a dash and a number, like a full version does.
 * We therefore refuse a @version that matches the version of one installed
 * component, and the full version of a different one.
 */
gboolean
mfd_store_check_version_is_full(MfStore *store,
                                const char *component_id,
                                const char *version,
                                gboolean *ret_full,
                                GError **error)
{
    g_autoptr(GPtrArray) components = NULL;
    gboolean as_version = FALSE;
    gboolean as_full = FALSE;

    *ret_full = FALSE;
    if (version == NULL)
        return TRUE;
    components = mf_store_list_components(store, component_id, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return FALSE;
    for (guint i = 0; i < components->len; i++) {
        auto component = MF_COMPONENT(g_ptr_array_index(components, i));

        as_version |= mfd_component_matches_version(component, version, FALSE);
        as_full |= mfd_component_matches_version(component, version, TRUE);
    }
    if (as_version && as_full) {
        g_set_error(
            error,
            MF_ERROR,
            MF_ERROR_FAILED,
            "%s %s is ambiguous: It matches both the version of an installed component, as well as its version and revision",
            component_id,
            version);
        return FALSE;
    }

    *ret_full = as_full;
    return TRUE;
}

gboolean
mfd_is_directory(const char *path)
{
    return g_file_test(path, G_FILE_TEST_IS_DIR);
}

gboolean
mfd_is_symlink(const char *path)
{
    return g_file_test(path, G_FILE_TEST_IS_SYMLINK);
}

gboolean
mfd_is_hidden(const char *name)
{
    return name[0] == '.';
}

/**
 * mfd_remove_if_exists:
 *
 * Remove a file, a link or a directory with everything in it.
 */
gboolean
mfd_remove_if_exists(const char *path, GError **error)
{
    return glnx_shutil_rm_rf_at(AT_FDCWD, path, NULL, error);
}

/**
 * mfd_unlink_if_exists:
 *
 * Remove a file or a link, but never a directory.
 */
gboolean
mfd_unlink_if_exists(const char *path, GError **error)
{
    if (unlink(path) == 0 || errno == ENOENT)
        return TRUE;
    glnx_throw_errno_prefix(error, "Unable to remove %s", path);
    return FALSE;
}

/**
 * mfd_make_dir:
 */
gboolean
mfd_make_dir(const char *path, GError **error)
{
    if (g_mkdir_with_parents(path, 0755) < 0) {
        glnx_throw_errno_prefix(error, "Unable to create directory %s", path);
        return FALSE;
    }
    return TRUE;
}

/**
 * mfd_make_symlink:
 */
gboolean
mfd_make_symlink(const char *target, const char *path, GError **error)
{
    if (symlink(target, path) < 0) {
        glnx_throw_errno_prefix(error, "Unable to create link %s", path);
        return FALSE;
    }
    return TRUE;
}

/**
 * mfd_check_plain_name:
 *
 * Check that a name is a single path segment, so it can not point outside
 * of its directory.
 */
static gboolean
mfd_check_plain_name(const char *name, GError **error)
{
    if (name[0] == '\0' || strchr(name, '/') != NULL || g_str_equal(name, ".") || g_str_equal(name, ".."))
        return glnx_throw(error, "'%s' is not the name of a file", name);
    return TRUE;
}

/**
 * mfd_replace_link_at:
 * @dfd: the directory of the link
 * @name: the name of the link, which can not have a slash
 * @target: the target of the link
 *
 * Create a link, replacing an existing file or link with the same name.
 * The link is replaced atomically, so a valid link with that name exists at
 * every moment.
 */
gboolean
mfd_replace_link_at(int dfd, const char *name, const char *target, GError **error)
{
    g_autofree char *tmp_name = g_strconcat(MF_TMP_PREFIX, name, NULL);

    if (!mfd_check_plain_name(name, error))
        return FALSE;
    if (unlinkat(dfd, tmp_name, 0) < 0 && errno != ENOENT)
        return glnx_throw_errno_prefix(error, "Unable to remove %s", tmp_name);
    if (symlinkat(target, dfd, tmp_name) < 0)
        return glnx_throw_errno_prefix(error, "Unable to create link %s", tmp_name);
    if (renameat(dfd, tmp_name, dfd, name) < 0) {
        glnx_throw_errno_prefix(error, "Unable to rename %s", tmp_name);
        unlinkat(dfd, tmp_name, 0);
        return FALSE;
    }

    return TRUE;
}

/**
 * mfd_replace_link:
 *
 * Create a link in a directory, like mfd_replace_link_at() does.
 */
gboolean
mfd_replace_link(const char *target, const char *dir, const char *name, GError **error)
{
    g_autofd int dfd = -1;

    if (!glnx_opendirat(AT_FDCWD, dir, TRUE, &dfd, error))
        return FALSE;
    return mfd_replace_link_at(dfd, name, target, error);
}

/**
 * mfd_make_dir_below:
 * @dfd: a directory
 * @subdir: a relative path
 *
 * Create a directory below @dfd together with its parent directories, like
 * `mkdir -p` does. We refuse a path that leads out of @dfd, and a path that
 * contains a symbolic link. The kernel enforces both restrictions if it
 * supports openat2().
 *
 * Returns: an `O_PATH` file descriptor of the directory, or -1 on error
 */
int
mfd_make_dir_below(int dfd, const char *subdir, GError **error)
{
    return glnx_chase_and_mkdirat(dfd,
                                  subdir,
                                  GLNX_CHASE_RESOLVE_BENEATH | GLNX_CHASE_RESOLVE_NO_SYMLINKS,
                                  0755,
                                  error);
}

/**
 * mfd_write_file_at:
 * @dfd: the directory of the file
 * @name: the name of the file, which can not have a slash
 * @mode: the permissions of the file
 *
 * Write a file, replacing an existing file with the same name. We write a new
 * file and rename it, so an existing link with that name is replaced and never
 * followed.
 */
gboolean
mfd_write_file_at(int dfd, const char *name, const void *content, size_t length, mode_t mode, GError **error)
{
    if (!mfd_check_plain_name(name, error))
        return FALSE;
    return glnx_file_replace_contents_with_perms_at(dfd,
                                                    name,
                                                    content,
                                                    length,
                                                    mode,
                                                    (uid_t) -1,
                                                    (gid_t) -1,
                                                    0,
                                                    NULL,
                                                    error);
}

/**
 * mfd_write_contents_safe:
 *
 * Write a small file into a directory, like mfd_write_file_at() does.
 */
gboolean
mfd_write_contents_safe(const char *dir,
                        const char *name,
                        const char *content,
                        ssize_t length,
                        GError **error)
{
    g_autofd int dfd = -1;

    if (!glnx_opendirat(AT_FDCWD, dir, TRUE, &dfd, error))
        return FALSE;
    return mfd_write_file_at(dfd, name, content, length < 0 ? strlen(content) : (size_t) length, 0644, error);
}

/**
 * mfd_write_word_safe:
 *
 * Write a marker file that holds one word.
 */
gboolean
mfd_write_word_safe(const char *dir, const char *name, const char *word, GError **error)
{
    g_autofree char *line = g_strconcat(word, "\n", NULL);

    return mfd_write_contents_safe(dir, name, line, -1, error);
}
