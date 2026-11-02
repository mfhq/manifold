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
 * @result: (nullable): the result of the operation that this is about
 * @format: a printf() format for the message
 *
 * Log a problem that does not fail an operation. If a caller is waiting for
 * the operation, the message also reaches it through @result.
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
 * Check that a name can only refer to something in the directory it is used
 * in, so that a file we create by that name stays in it.
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
 * @target: what the link points to
 *
 * Create a link, replacing what is there, so that there is a valid link by
 * that name at every moment.
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
    glnx_autofd int dfd = -1;

    if (!glnx_opendirat(AT_FDCWD, dir, TRUE, &dfd, error))
        return FALSE;
    return mfd_replace_link_at(dfd, name, target, error);
}

/**
 * mfd_make_dir_below:
 * @dfd: a directory
 * @subdir: a relative path
 *
 * Create a directory below @dfd with the directories it is in, as `mkdir -p`
 * would. We refuse a path that leads out of @dfd, and every link on the way to
 * the directory, which the kernel enforces where it supports openat2().
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
 * Write a file, replacing what is there. We write a new file and rename it, so
 * a link of that name is replaced, and never followed.
 */
gboolean
mfd_write_file_at(int dfd, const char *name, const void *content, gsize length, mode_t mode, GError **error)
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
mfd_write_contents_safe(const char *dir, const char *name, const char *content, gssize length, GError **error)
{
    glnx_autofd int dfd = -1;

    if (!glnx_opendirat(AT_FDCWD, dir, TRUE, &dfd, error))
        return FALSE;
    return mfd_write_file_at(dfd, name, content, length < 0 ? strlen(content) : (gsize) length, 0644, error);
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
