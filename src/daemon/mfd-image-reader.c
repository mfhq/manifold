/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include "config.h"
#include "mfd-image-reader.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <gio/gio.h>

#include "mf-store-private.h"

/*
 * Reading the files an image wants to install on the host, without mounting it.
 *
 * Only manifold-rund mounts images, and installing should not depend on it.
 * So we let dump.erofs find a file in the filesystem of the image.
 * We run that tool with as little permissions as possible and with tight limits,
 * to guard a bit against bad images doing bad things at install-time (the image will
 * be fully mounted by rund as soon as any app is launched though, so this is not
 * a security protection, but just guards against regular bugs).
 *
 * NOTE: We must only run dump.erofs on verified images, and never on untrusted input.
 * So we only read images from the store, as everything in there came through a
 * complete chain of trust.
 */

/* the tool that prints a file of an image, which erofs-utils can do since 1.8.5 */
#define DUMP_TOOL "dump.erofs"

/* the exit status of dump.erofs if we could not set it up */
constexpr int READER_SETUP_FAILED = 125;

/* limits for the files an image can install, which are metadata and icons */
constexpr gsize FILE_SIZE_MAX = 8 * 1024 * 1024;
constexpr gsize TOTAL_SIZE_MAX = 64 * 1024 * 1024;
constexpr guint FILES_MAX = 1024;

/* limits for dump.erofs, which usually takes milliseconds to print a file */
constexpr unsigned int READER_TIMEOUT_SEC = 30;
constexpr rlim_t READER_CPU_SEC = 10;
constexpr rlim_t READER_MEMORY_MAX = 1024 * 1024 * 1024;

/**
 * MfdErofsReaderUser:
 *
 * The user that dump.erofs runs as. We look the user up before starting the
 * tool, as that is not safe to do between fork and exec.
 */
typedef struct {
    gboolean drop_root;
    uid_t uid;
    gid_t gid;
} MfdErofsReaderUser;

/**
 * mfd_is_host_root:
 *
 * Check if we are root of the host, and not just of a user namespace
 * (like in a container or in our tests, which does not have a user to change to).
 *
 * returns: %TRUE if we are true root.
 */
static gboolean
mfd_is_host_root(void)
{
    g_autofree char *map = NULL;
    unsigned int inside = 0;
    unsigned int outside = 0;
    unsigned int count = 0;

    if (geteuid() != 0)
        return FALSE;
    /* if in doubt, we assume we are root, as then we fail to run the tool instead of running it as root */
    if (!g_file_get_contents("/proc/self/uid_map", &map, NULL, NULL) ||
        sscanf(map, "%u %u %u", &inside, &outside, &count) != 3)
        return TRUE;

    return inside == 0 && outside == 0 && count == G_MAXUINT32;
}

/**
 * mfd_reader_user_init:
 *
 * Decide which user dump.erofs runs as.
 */
static gboolean
mfd_reader_user_init(MfdErofsReaderUser *user, GError **error)
{
    const struct passwd *nobody;

    user->drop_root = mfd_is_host_root();
    if (!user->drop_root)
        return TRUE;

    nobody = getpwnam("nobody");
    if (nobody == NULL) {
        g_set_error_literal(error, MF_ERROR, MF_ERROR_FAILED, "There is no user 'nobody' to read images as");
        return FALSE;
    }
    user->uid = nobody->pw_uid;
    user->gid = nobody->pw_gid;

    return TRUE;
}

/**
 * mfd_reader_child_setup:
 *
 * Limit dump.erofs, between fork and exec. If anything fails here, the tool does
 * not run, which leaves us without a file that matches its checksums.
 *
 * The alarm timer stays set across the exec, and kills dump.erofs if it does not
 * finish in time.
 */
static void
mfd_reader_child_setup(gpointer user_data)
{
    const MfdErofsReaderUser *user = user_data;
    const struct rlimit cpu = {READER_CPU_SEC, READER_CPU_SEC};
    const struct rlimit memory = {READER_MEMORY_MAX, READER_MEMORY_MAX};
    const struct rlimit none = {0, 0};

    alarm(READER_TIMEOUT_SEC);
    if (setrlimit(RLIMIT_CPU, &cpu) != 0 || setrlimit(RLIMIT_AS, &memory) != 0 ||
        setrlimit(RLIMIT_FSIZE, &none) != 0 || setrlimit(RLIMIT_CORE, &none) != 0)
        _exit(READER_SETUP_FAILED);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        _exit(READER_SETUP_FAILED);

    if (user->drop_root && (setgroups(0, nullptr) != 0 || setresgid(user->gid, user->gid, user->gid) != 0 ||
                            setresuid(user->uid, user->uid, user->uid) != 0))
        _exit(READER_SETUP_FAILED);
}

/**
 * mfd_reader_find_tool:
 *
 * Returns: (transfer full): the path of a tool that can print files of an image, or %NULL
 */
static char *
mfd_reader_find_tool(GError **error)
{
    g_autofree char *tool = g_find_program_in_path(DUMP_TOOL);
    g_autofree char *help = NULL;
    g_autoptr(GSubprocess) proc = NULL;

    if (tool == NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "%s of erofs-utils is needed to read files from images, but it is not installed",
                    DUMP_TOOL);
        return NULL;
    }

    proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE,
                            error,
                            tool,
                            "--help",
                            NULL);
    if (proc == NULL || !g_subprocess_communicate_utf8(proc, NULL, NULL, &help, NULL, error))
        return NULL;
    if (strstr(help, "--cat") == NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "%s of erofs-utils is too old to read files from images, version 1.8.5 is needed",
                    DUMP_TOOL);
        return NULL;
    }

    return g_steal_pointer(&tool);
}

/**
 * mfd_reader_read_file:
 * @tool: the tool that reads the image
 * @path: where the file is in the image
 *
 * Returns: (transfer full): the file's data, or %NULL on error. The caller has to verify the output.
 */
static GBytes *
mfd_reader_read_file(const char *tool,
                     const char *image_path,
                     guint64 data_offset,
                     const char *path,
                     const MfdErofsReaderUser *user,
                     GError **error)
{
    g_autofree char *offset_arg = g_strdup_printf("--offset=%" G_GUINT64_FORMAT, data_offset);
    g_autofree char *path_arg = g_strconcat("--path=", path, NULL);
    const char *const argv[] = {tool, offset_arg, "--cat", path_arg, image_path, NULL};
    const char *const env[] = {"LC_ALL=C", NULL};
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GByteArray) content = g_byte_array_new();
    GInputStream *output;
    guint8 buffer[64 * 1024];

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_set_environ(launcher, (char **) env);
    g_subprocess_launcher_set_cwd(launcher, "/");
    g_subprocess_launcher_set_child_setup(launcher, mfd_reader_child_setup, (gpointer) user, NULL);
    proc = g_subprocess_launcher_spawnv(launcher, argv, error);
    if (proc == NULL)
        return NULL;

    output = g_subprocess_get_stdout_pipe(proc);
    for (;;) {
        gssize n = g_input_stream_read(output, buffer, sizeof(buffer), NULL, error);

        if (n < 0) {
            g_subprocess_force_exit(proc);
            g_subprocess_wait(proc, NULL, NULL);
            return NULL;
        }
        if (n == 0)
            break;

        g_byte_array_append(content, buffer, (guint) n);
        if (content->len > FILE_SIZE_MAX) {
            g_subprocess_force_exit(proc);
            g_subprocess_wait(proc, NULL, NULL);
            g_set_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST, "%s is too large to be installed", path);
            return NULL;
        }
    }
    g_subprocess_wait(proc, NULL, NULL);

    return g_byte_array_free_to_bytes(g_steal_pointer(&content));
}

/**
 * mfd_image_read_integration:
 * @store: the store that holds the image
 * @manifest: the manifest of the image, which passed mf_manifest_check_integration()
 * @error: a #GError
 *
 * Read the files an image wants to install, and check that they are what its
 * manifest says they are. The image has to be in @store.
 *
 * Returns: (transfer full) (element-type utf8 GBytes): the files by their
 *   export path, or %NULL
 */
GHashTable *
mfd_image_read_integration(MfStore *store, MfManifest *manifest, GError **error)
{
    MfIntegration *integration = mf_manifest_get_integration(manifest);
    const char *verity_hash = mf_manifest_get_verity_hash(manifest);
    guint64 data_offset = mf_manifest_get_data_offset(manifest);
    GPtrArray *files;
    g_autoptr(GHashTable) contents = NULL;
    g_autofree char *image_path = NULL;
    g_autofree char *tool = NULL;
    MfdErofsReaderUser user = {};
    gsize total_size = 0;

    contents = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
    if (integration == NULL)
        return g_steal_pointer(&contents);
    files = mf_integration_get_files(integration);
    if (files->len == 0)
        return g_steal_pointer(&contents);
    if (files->len > FILES_MAX) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_MANIFEST,
                    "The image wants to install too many files: %u, of %u at most",
                    files->len,
                    FILES_MAX);
        return NULL;
    }

    /* everything in the store was verified, so these are the only images we can read */
    if (!mf_store_has_image(store, verity_hash)) {
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, "Image %s is not in the store", verity_hash);
        return NULL;
    }
    image_path = mf_store_image_path_at(mf_store_get_path(store), verity_hash);

    tool = mfd_reader_find_tool(error);
    if (tool == NULL || !mfd_reader_user_init(&user, error))
        return NULL;

    for (guint i = 0; i < files->len; i++) {
        MfIntegrationFile *file = g_ptr_array_index(files, i);
        const char *path = mf_integration_file_get_path(file);
        g_autoptr(GBytes) content = mfd_reader_read_file(tool, image_path, data_offset, path, &user, error);

        if (content == NULL)
            return NULL;
        /* dump.erofs prints nothing for a missing file, and a manifest could record the hash of empty data */
        if (g_bytes_get_size(content) == 0) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_VERIFICATION_FAILED,
                        "%s is empty or not in the image",
                        path);
            return NULL;
        }
        if (!mf_integration_file_check_content(file, content)) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_VERIFICATION_FAILED,
                        "%s in the image does not match what its manifest records",
                        path);
            return NULL;
        }

        total_size += g_bytes_get_size(content);
        if (total_size > TOTAL_SIZE_MAX) {
            g_set_error_literal(error,
                                MF_ERROR,
                                MF_ERROR_INVALID_MANIFEST,
                                "The files the image wants to install are too large");
            return NULL;
        }
        g_hash_table_insert(contents,
                            g_strdup(mf_integration_file_get_export_path(file)),
                            g_steal_pointer(&content));
    }

    return g_steal_pointer(&contents);
}
