/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include "config.h"
#include "mfr-environment.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <libglnx.h>

#include "mf-constants-private.h"

/*
 * The environment files of the runtimes.
 *
 * A core, a platform or a kit can set environment variables for the
 * applications that run on it, with files in MF_TREE_ENVIRONMENT_DIR. They
 * work like the ones of systemd's environment.d: every file has lines of
 * NAME=VALUE, the files of all layers are read in the order of their names,
 * and a file replaces the one of the same name in a layer below.
 *
 * A value can refer to the environment of the caller, as ${NAME}. We do not
 * know that environment, so we hand the values on as they are, and the
 * launcher expands them.
 */

/* the suffix of the files we read */
#define ENVIRONMENT_FILE_SUFFIX ".conf"

/* no environment file is anywhere near this size, so a larger one is a mistake */
constexpr size_t ENVIRONMENT_FILE_SIZE_MAX = 64 * 1024;

/**
 * mfr_environment_open:
 *
 * Open a path of a layer. Links are resolved with the layer as root
 * directory, as they are in the tree, so none leads out of the image.
 */
static int
mfr_environment_open(int root_fd, const char *path, int flags)
{
    struct open_how how = {.flags = flags | O_CLOEXEC, .resolve = RESOLVE_IN_ROOT};

    return (int) syscall(SYS_openat2, root_fd, path, &how, sizeof(how));
}

/**
 * mfr_environment_find_files:
 * @files: (element-type utf8 int): the layer that has each file, by the name of the file
 *
 * Add the environment files of a layer, replacing those of the layers below
 * that have the same name.
 */
static void
mfr_environment_find_files(const char *layer_root, int layer, GHashTable *files)
{
    g_autofd int root_fd = open(layer_root, O_PATH | O_DIRECTORY | O_CLOEXEC);
    g_autofd int dir_fd = -1;
    DIR *dir = NULL;
    struct dirent *entry;

    if (root_fd < 0) {
        g_warning("Unable to open %s: %s", layer_root, g_strerror(errno));
        return;
    }
    dir_fd = mfr_environment_open(root_fd, MF_TREE_ENVIRONMENT_DIR, O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        if (errno != ENOENT && errno != ENOTDIR)
            g_warning("Unable to open %s/%s: %s", layer_root, MF_TREE_ENVIRONMENT_DIR, g_strerror(errno));
        return;
    }

    dir = fdopendir(dir_fd);
    if (dir == NULL)
        return;
    dir_fd = -1; /* belongs to the directory stream now */
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !g_str_has_suffix(entry->d_name, ENVIRONMENT_FILE_SUFFIX))
            continue;
        g_hash_table_insert(files, g_strdup(entry->d_name), GINT_TO_POINTER(layer));
    }
    closedir(dir);
}

/**
 * mfr_environment_read_file:
 *
 * Returns: (transfer full) (nullable): the contents of an environment file,
 *     or %NULL for one that is not to be used
 */
static char *
mfr_environment_read_file(const char *layer_root, const char *name)
{
    g_autofree char *path = g_build_filename(MF_TREE_ENVIRONMENT_DIR, name, NULL);
    g_autofd int root_fd = open(layer_root, O_PATH | O_DIRECTORY | O_CLOEXEC);
    g_autofd int fd = -1;
    g_autofree char *content = NULL;
    struct stat st;
    size_t length = 0;

    if (root_fd >= 0)
        fd = mfr_environment_open(root_fd, path, O_RDONLY | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &st) != 0) {
        g_warning("Unable to read %s/%s: %s", layer_root, path, g_strerror(errno));
        return NULL;
    }
    /* A kit is the upper directory of an overlay, so a file that it removed
     * from its base is a character device there. Anything that is no file
     * hides the file of the layers below, and sets nothing itself. */
    if (!S_ISREG(st.st_mode))
        return NULL;
    if ((size_t) st.st_size > ENVIRONMENT_FILE_SIZE_MAX) {
        g_warning("Ignoring %s/%s: the file is too large", layer_root, path);
        return NULL;
    }

    content = g_malloc(st.st_size + 1);
    while (length < (size_t) st.st_size) {
        ssize_t n = read(fd, content + length, st.st_size - length);

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            g_warning("Unable to read %s/%s: %s", layer_root, path, g_strerror(errno));
            return NULL;
        }
        if (n == 0)
            break;
        length += n;
    }
    content[length] = '\0';
    if (strlen(content) != length) {
        g_warning("Ignoring %s/%s: the file is not text", layer_root, path);
        return NULL;
    }

    return g_steal_pointer(&content);
}

/**
 * mfr_environment_name_is_valid:
 *
 * Returns: whether @name can be the name of an environment variable
 */
static gboolean
mfr_environment_name_is_valid(const char *name)
{
    if (!g_ascii_isalpha(name[0]) && name[0] != '_')
        return FALSE;
    for (const char *c = name + 1; *c != '\0'; c++) {
        if (!g_ascii_isalnum(*c) && *c != '_')
            return FALSE;
    }

    return TRUE;
}

/**
 * mfr_environment_parse:
 * @name: the name of the file, for messages
 *
 * Add the variables that the contents of an environment file set. A line we
 * can not use is skipped: an application should still start if one of its
 * runtimes has a mistake in such a file.
 */
static void
mfr_environment_parse(const char *name, const char *content, GStrvBuilder *builder)
{
    g_auto(GStrv) lines = g_strsplit(content, "\n", -1);

    for (size_t i = 0; lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]);
        char *value = strchr(line, '=');

        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (value == NULL) {
            g_warning("Ignoring line %zu of %s: it is not NAME=VALUE", i + 1, name);
            continue;
        }
        *value++ = '\0';
        g_strchomp(line);
        g_strchug(value);
        if (!mfr_environment_name_is_valid(line)) {
            g_warning("Ignoring line %zu of %s: '%s' is no name of a variable", i + 1, name, line);
            continue;
        }
        /* the launcher builds the search path itself, from the one of the caller */
        if (g_str_equal(line, "PATH")) {
            g_warning("Ignoring line %zu of %s: PATH can not be set", i + 1, name);
            continue;
        }

        g_strv_builder_take(builder, g_strdup_printf("%s=%s", line, value));
    }
}

/**
 * mfr_environment_load:
 * @layer_roots: (array zero-terminated=1): where the runtimes of an
 *     application are mounted, from the core upwards
 *
 * Read the environment files of the runtimes of an application.
 *
 * Returns: (transfer full): the variables to set, as NAME=VALUE in the order
 *     in which they have to be set. The values are not expanded.
 */
char **
mfr_environment_load(const char *const *layer_roots)
{
    g_autoptr(GHashTable) files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
    g_autoptr(GPtrArray) names = NULL;

    for (int layer = 0; layer_roots[layer] != NULL; layer++)
        mfr_environment_find_files(layer_roots[layer], layer, files);

    names = g_hash_table_get_keys_as_ptr_array(files);
    g_ptr_array_sort_values(names, (GCompareFunc) strcmp);
    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        int layer = GPOINTER_TO_INT(g_hash_table_lookup(files, name));
        g_autofree char *content = mfr_environment_read_file(layer_roots[layer], name);

        if (content != NULL)
            mfr_environment_parse(name, content, builder);
    }

    return g_strv_builder_end(builder);
}
