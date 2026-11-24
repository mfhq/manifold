/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/capability.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

#include "mf-constants-private.h"
#include "mf-host-links.h"
#include "mf-run-interface.h"

/*
 * mfrun is the runner that users call. It asks the compose service
 * for the mount tree of a component, and launches the component in it.
 *
 * This tool is unprivileged and runs as the invoking user. It moves its own
 * process into the mount tree and executes the application in place.
 *
 * Changing the mount namespace of a process needs CAP_SYS_ADMIN. If we
 * already have that (e.g. if we were executed by a systemd service), we just
 * move into the tree directly, and keep everything we started with. If we
 * don't have capabilities, we create a user namespace first.
 */

#define PROGRAM_NAME "mfrun"

/* the search path we use if the caller has none */
#define DEFAULT_SEARCH_PATH "/usr/local/bin:/usr/bin:/bin"

/*
 * The exit statuses that we return if we can not run the program. Shells and
 * env(1) use the same values. If the program runs, we exit with its exit status.
 */
enum {
    MFX_EXIT_FAILED = 125,         /* we failed ourselves, e.g. the component is not installed */
    MFX_EXIT_CANNOT_EXECUTE = 126, /* the program is there, but can not be executed */
    MFX_EXIT_NOT_FOUND = 127,      /* there is no such program */
};

/**
 * MfxHostCommands:
 *
 * The directories that are needed to run programs of the host. The compose
 * service only sends them for an application that has the permission to run
 * programs of the host.
 */
typedef struct {
    char *directory;     /* the directory with the links in the tree, or nullptr if there is none */
    char **system_paths; /* the program directories of the host that the links were created for */
} MfxHostCommands;

/**
 * MfxLaunchData:
 *
 * The data that the compose service returns for a launch.
 */
typedef struct {
    unsigned int mount_fd_idx;     /* index of the file descriptor of the tree in the reply */
    int mount_fd;                  /* file descriptor of the tree, or -1 if not received yet */
    char *executable;              /* absolute path of the program within the tree */
    char **environment;            /* NAME=VALUE entries, an empty VALUE means the variable is unset */
    MfxHostCommands host_commands; /* only set if the application may run programs of the host */
} MfxLaunchData;

/**
 * mfx_strv_free:
 */
static void
mfx_strv_free(char **strv)
{
    if (strv == nullptr)
        return;
    for (char **s = strv; *s != nullptr; s++)
        free(*s);
    free(strv);
}

/**
 * mfx_launch_data_clear:
 */
static void
mfx_launch_data_clear(MfxLaunchData *launch_data)
{
    if (launch_data->mount_fd >= 0)
        close(launch_data->mount_fd);
    launch_data->mount_fd = -1;
    free(launch_data->executable);
    launch_data->executable = nullptr;
    mfx_strv_free(launch_data->environment);
    launch_data->environment = nullptr;
    free(launch_data->host_commands.directory);
    launch_data->host_commands.directory = nullptr;
    mfx_strv_free(launch_data->host_commands.system_paths);
    launch_data->host_commands.system_paths = nullptr;
}

/**
 * mfx_print_usage:
 */
static void
mfx_print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: %s [OPTIONS] <component-id> [args...]\n"
            "\n"
            "Options:\n"
            "  --socket=PATH      Connect to the compose service at PATH\n"
            "                     (default: %s)\n"
            "  --command=PROGRAM  Run PROGRAM in the environment of the component,\n"
            "                     instead of the component itself\n"
            "  --help             Show this help\n"
            "  --version          Show the program version\n"
            "\n"
            "Exit status:\n"
            "  The exit status of the program, or\n"
            "  125  if %s failed itself\n"
            "  126  if the program can not be executed\n"
            "  127  if there is no such program\n",
            PROGRAM_NAME,
            MF_RUN_SOCKET_PATH,
            PROGRAM_NAME);
}

/**
 * mfx_print_connection_error:
 */
static void
mfx_print_connection_error(const char *socket_path, int negative_errno)
{
    fprintf(stderr,
            "%s: Unable to reach the Manifold compose service at %s: %s\n",
            PROGRAM_NAME,
            socket_path,
            strerror(-negative_errno));
}

/**
 * mfx_print_service_error:
 */
static void
mfx_print_service_error(const char *component_id, const char *error_id, sd_json_variant *parameters)
{
    if (strcmp(error_id, MF_RUN_ERROR_NO_SUCH_COMPONENT_ID) == 0) {
        fprintf(stderr, "%s: Component %s is not installed\n", PROGRAM_NAME, component_id);
    } else if (strcmp(error_id, MF_RUN_ERROR_COMPONENT_REVOKED_ID) == 0) {
        fprintf(stderr, "%s: Component %s was revoked and can not be launched\n", PROGRAM_NAME, component_id);
    } else if (strcmp(error_id, MF_RUN_ERROR_COMPOSITION_FAILED_ID) == 0) {
        sd_json_variant *message = sd_json_variant_by_key(parameters, "message");

        fprintf(stderr,
                "%s: Unable to prepare component %s: %s\n",
                PROGRAM_NAME,
                component_id,
                sd_json_variant_is_string(message) ? sd_json_variant_string(message) : "unknown error");
    } else if (strcmp(error_id, SD_VARLINK_ERROR_INVALID_PARAMETER) == 0) {
        fprintf(stderr, "%s: Component identifier is not valid: %s\n", PROGRAM_NAME, component_id);
    } else if (strcmp(error_id, SD_VARLINK_ERROR_PERMISSION_DENIED) == 0) {
        fprintf(stderr, "%s: Not permitted to launch component %s\n", PROGRAM_NAME, component_id);
    } else {
        fprintf(stderr,
                "%s: The Manifold compose service failed to prepare component %s: %s\n",
                PROGRAM_NAME,
                component_id,
                error_id);
    }
}

/**
 * mfx_dispatch_host_commands:
 *
 * Read the hostCommands field of the reply of the compose service.
 */
static int
mfx_dispatch_host_commands(const char *name,
                           sd_json_variant *variant,
                           sd_json_dispatch_flags_t flags,
                           void *userdata)
{
    static const sd_json_dispatch_field dispatch_table[] = {
        {"directory",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_string,
         offsetof(MfxHostCommands, directory),
         SD_JSON_MANDATORY},
        {"systemPaths",
         SD_JSON_VARIANT_ARRAY, sd_json_dispatch_strv,
         offsetof(MfxHostCommands, system_paths),
         0},
        {},
    };

    if (sd_json_variant_is_null(variant))
        return 0;
    return sd_json_dispatch(variant, dispatch_table, flags, userdata);
}

/**
 * mfx_prepare_launch:
 *
 * Ask the compose service for the mount tree and the launch data of a component.
 *
 * Returns: 0 on success, or a negative errno. An error message was printed
 *   already if the call failed.
 */
static int
mfx_prepare_launch(const char *socket_path, const char *component_id, MfxLaunchData *launch_data)
{
    static const sd_json_dispatch_field dispatch_table[] = {
        {"mountFileDescriptor",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint,
         offsetof(MfxLaunchData, mount_fd_idx),
         SD_JSON_MANDATORY},
        {"executable",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_string,
         offsetof(MfxLaunchData, executable),
         SD_JSON_MANDATORY},
        {"environment",
         SD_JSON_VARIANT_ARRAY, sd_json_dispatch_strv,
         offsetof(MfxLaunchData, environment),
         0},
        {"hostCommands",
         SD_JSON_VARIANT_OBJECT, mfx_dispatch_host_commands,
         offsetof(MfxLaunchData, host_commands),
         SD_JSON_NULLABLE},
        {},
    };
    sd_varlink *link = nullptr;
    sd_json_variant *parameters = nullptr;
    const char *error_id = nullptr;
    int r;

    r = sd_varlink_connect_address(&link, socket_path);
    if (r < 0) {
        mfx_print_connection_error(socket_path, r);
        goto out;
    }

    r = sd_varlink_set_allow_fd_passing_input(link, true);
    if (r < 0) {
        fprintf(stderr, "%s: Unable to allow receiving file descriptors: %s\n", PROGRAM_NAME, strerror(-r));
        goto out;
    }

    r = sd_varlink_callbo(link,
                          MF_RUN_METHOD_PREPARE_LAUNCH,
                          &parameters,
                          &error_id,
                          SD_JSON_BUILD_PAIR_STRING("componentId", component_id));
    if (r < 0) {
        mfx_print_connection_error(socket_path, r);
        goto out;
    }
    if (error_id != nullptr) {
        mfx_print_service_error(component_id, error_id, parameters);
        r = -EIO;
        goto out;
    }

    r = sd_json_dispatch(parameters, dispatch_table, SD_JSON_ALLOW_EXTENSIONS, launch_data);
    if (r < 0) {
        fprintf(stderr,
                "%s: Failed to parse reply of the Manifold compose service: %s\n",
                PROGRAM_NAME,
                strerror(-r));
        goto out;
    }

    if (launch_data->executable[0] != '/') {
        fprintf(stderr,
                "%s: Received a relative executable path (this is not allowed): %s\n",
                PROGRAM_NAME,
                launch_data->executable);
        r = -EINVAL;
        goto out;
    }

    if (launch_data->host_commands.directory != nullptr && launch_data->host_commands.directory[0] != '/') {
        fprintf(stderr,
                "%s: Received a relative directory for the programs of the host (this is not allowed): %s\n",
                PROGRAM_NAME,
                launch_data->host_commands.directory);
        r = -EINVAL;
        goto out;
    }

    launch_data->mount_fd = sd_varlink_take_fd(link, launch_data->mount_fd_idx);
    if (launch_data->mount_fd < 0) {
        fprintf(stderr, "%s: Did not receive a mount tree\n", PROGRAM_NAME);
        r = -EBADF;
        goto out;
    }
    r = 0;

out:
    if (r < 0)
        mfx_launch_data_clear(launch_data);
    /* the reply parameters are owned by the connection, so we do not free them */
    sd_varlink_close_unref(link);

    return r;
}

/**
 * mfx_have_cap_sys_admin:
 *
 * Check if our current process has CAP_SYS_ADMIN.
 */
static bool
mfx_have_cap_sys_admin(void)
{
    struct __user_cap_header_struct header = {.version = _LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3] = {};

    if (syscall(SYS_capget, &header, data) != 0)
        return false;

    return (data[CAP_TO_INDEX(CAP_SYS_ADMIN)].effective & CAP_TO_MASK(CAP_SYS_ADMIN)) != 0;
}

/**
 * mfx_write_proc_file:
 *
 * Write a string to a file in /proc/self
 */
static int
mfx_write_proc_file(const char *path, const char *content)
{
    int fd;
    ssize_t written;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    written = write(fd, content, strlen(content));
    if (written < 0) {
        int saved_errno = errno;

        close(fd);
        return -saved_errno;
    }
    close(fd);

    return 0;
}

/**
 * mfx_print_userns_hint:
 *
 * Print a hint if AppArmor may be the reason why we can not use the
 * capabilities of our user namespace. If AppArmor restricts unprivileged user
 * namespaces, an unconfined program that creates a user namespace is moved
 * to a profile that denies all capabilities in the namespace.
 */
static void
mfx_print_userns_hint(int negative_errno)
{
    char value = '\0';
    int fd;

    if (negative_errno != -EPERM && negative_errno != -EACCES)
        return;

    fd = open("/proc/sys/kernel/apparmor_restrict_unprivileged_userns", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    if (read(fd, &value, 1) != 1)
        value = '\0';
    close(fd);
    if (value != '1')
        return;

    fprintf(stderr,
            "%s: AppArmor restricts unprivileged user namespaces on this host, and does not permit\n"
            "%s to use them. Make sure its AppArmor profile is installed and loaded.\n",
            PROGRAM_NAME,
            PROGRAM_NAME);
}

/**
 * mfx_map_self:
 *
 * Map our uid and gid to the same IDs in the new user namespace. No other
 * IDs are mapped.
 */
static int
mfx_map_self(uid_t uid, gid_t gid)
{
    char mapping[64];
    int r;

    r = mfx_write_proc_file("/proc/self/setgroups", "deny");
    if (r < 0)
        return r;
    snprintf(mapping, sizeof(mapping), "%u %u 1", (unsigned int) uid, (unsigned int) uid);
    r = mfx_write_proc_file("/proc/self/uid_map", mapping);
    if (r < 0)
        return r;
    snprintf(mapping, sizeof(mapping), "%u %u 1", (unsigned int) gid, (unsigned int) gid);
    return mfx_write_proc_file("/proc/self/gid_map", mapping);
}

/**
 * mfx_enter_tree:
 *
 * Create a new mount namespace for our process, and make the mount tree its
 * root. If @user_namespace is set, we also create a user namespace, which
 * gives us the capabilities that are needed to change mounts.
 *
 * Returns: 0 on success, or a negative errno. An error message was printed
 *   already if the function failed.
 */
static int
mfx_enter_tree(int mount_fd, bool user_namespace)
{
    int flags = CLONE_NEWNS | (user_namespace ? CLONE_NEWUSER : 0);
    /* read our IDs before we create the user namespace, because they are unmapped in it at first */
    uid_t uid = getuid();
    gid_t gid = getgid();
    int r;

    if (unshare(flags) != 0) {
        r = -errno;
        if (user_namespace)
            fprintf(stderr,
                    "%s: Unable to create a user namespace: %s"
                    " (unprivileged user namespaces may be disabled on this host)\n",
                    PROGRAM_NAME,
                    strerror(-r));
        else
            fprintf(stderr, "%s: Unable to create a mount namespace: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }

    if (user_namespace) {
        r = mfx_map_self(uid, gid);
        if (r < 0) {
            fprintf(stderr,
                    "%s: Unable to map our user into the user namespace: %s\n",
                    PROGRAM_NAME,
                    strerror(-r));
            mfx_print_userns_hint(r);
            return r;
        }
    }

    /* make all mounts private, so our mount changes are not propagated to the host */
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        r = -errno;
        fprintf(stderr, "%s: Unable to make mounts private: %s\n", PROGRAM_NAME, strerror(-r));
        if (user_namespace)
            mfx_print_userns_hint(r);
        return r;
    }
    if (move_mount(mount_fd, "", AT_FDCWD, "/", MOVE_MOUNT_F_EMPTY_PATH) != 0) {
        r = -errno;
        fprintf(stderr, "%s: Unable to attach the mount tree: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }
    if (fchdir(mount_fd) != 0) {
        r = -errno;
        fprintf(stderr, "%s: Unable to change into the mount tree: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }
    /* pivot_root(".", ".") mounts the old root on top of the new root, so we can detach it via "." */
    if (syscall(SYS_pivot_root, ".", ".") != 0) {
        r = -errno;
        fprintf(stderr, "%s: Unable to pivot into the mount tree: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }
    if (umount2(".", MNT_DETACH) != 0) {
        r = -errno;
        fprintf(stderr, "%s: Unable to detach the old root: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }

    return 0;
}

/**
 * mfx_change_directory:
 *
 * Change to the working directory of the caller, if it exists in the tree.
 * Otherwise we change to the home directory of the caller, or to the root
 * of the tree.
 */
static void
mfx_change_directory(const char *cwd)
{
    if (cwd != nullptr && chdir(cwd) == 0)
        return;

    const char *home = getenv("HOME");
    if (home != nullptr && chdir(home) == 0)
        return;
    if (chdir("/") != 0)
        fprintf(stderr, "%s: Unable to change to the root of the tree: %s\n", PROGRAM_NAME, strerror(errno));
}

/**
 * mfx_apply_environment:
 *
 * Apply the environment variables that the compose service sent. A variable
 * with an empty value is unset.
 */
static void
mfx_apply_environment(char **environment)
{
    if (environment == nullptr)
        return;
    for (char **e = environment; *e != nullptr; e++) {
        char *separator = strchr(*e, '=');

        if (separator == nullptr)
            continue;
        *separator = '\0';
        if (separator[1] == '\0')
            unsetenv(*e);
        else
            setenv(*e, separator + 1, 1);
    }
}

/**
 * mfx_search_path_contains:
 * @search_path: directories, separated by colons
 * @dir: a directory, which does not have to be null-terminated
 * @dir_length: the length of @dir
 *
 * Returns: true if @dir is one of the directories of @search_path
 */
static bool
mfx_search_path_contains(const char *search_path, const char *dir, size_t dir_length)
{
    for (const char *entry = search_path; *entry != '\0';) {
        size_t length = strcspn(entry, ":");

        if (length == dir_length && strncmp(entry, dir, length) == 0)
            return true;
        entry += length;
        if (*entry == ':')
            entry++;
    }
    return false;
}

/**
 * mfx_search_path_add:
 * @search_path: (inout): directories, separated by colons, in a buffer with enough space for @dir
 * @dir: a directory, which does not have to be null-terminated
 * @dir_length: the length of @dir
 *
 * Add a directory to the end of a search path.
 */
static void
mfx_search_path_add(char *search_path, const char *dir, size_t dir_length)
{
    char *end = search_path + strlen(search_path);

    if (end != search_path)
        *end++ = ':';
    end = mempcpy(end, dir, dir_length);
    *end = '\0';
}

/**
 * mfx_clean_search_path:
 * @caller_path: (nullable): the PATH we were called with
 *
 * Clean up the PATH of the caller. We drop empty and relative entries,
 * because both make the current directory part of the search, and we drop
 * duplicate entries. We also drop the program directory of the application,
 * because we always add it ourselves.
 *
 * Returns: the cleaned search path, or nullptr if we are out of memory
 */
static char *
mfx_clean_search_path(const char *caller_path)
{
    if (caller_path == nullptr)
        caller_path = DEFAULT_SEARCH_PATH;

    /* we only drop entries, so the result is never longer than the PATH of the caller */
    char *search_path = calloc(1, strlen(caller_path) + 1);
    if (search_path == nullptr)
        return nullptr;

    for (const char *entry = caller_path; *entry != '\0';) {
        size_t length = strcspn(entry, ":");
        const char *next = entry + length + (entry[length] == ':' ? 1 : 0);

        /* "/usr/bin/" is the same directory as "/usr/bin" */
        while (length > 1 && entry[length - 1] == '/')
            length--;
        if (length > 0 && entry[0] == '/' && !mfx_search_path_contains(MF_TREE_APP_BIN_DIR, entry, length) &&
            !mfx_search_path_contains(search_path, entry, length))
            mfx_search_path_add(search_path, entry, length);
        entry = next;
    }

    return search_path;
}

/**
 * mfx_sync_user_links:
 * @links_fd: the directory with the links
 * @user_dirs: program directories of the host, separated by colons
 *
 * Update the links in a directory to match the programs in the given
 * program directories of the host. A directory that we can not read is skipped.
 *
 * Returns: 0 on success, or a negative errno
 */
static int
mfx_sync_user_links(int links_fd, const char *user_dirs)
{
    size_t n_dirs = 1;
    int *dir_fds;
    int r;

    for (const char *c = user_dirs; *c != '\0'; c++) {
        if (*c == ':')
            n_dirs++;
    }
    dir_fds = calloc(n_dirs, sizeof(int));
    if (dir_fds == nullptr)
        return -ENOMEM;

    n_dirs = 0;
    for (const char *entry = user_dirs; *entry != '\0';) {
        size_t length = strcspn(entry, ":");
        char dir[PATH_MAX];

        if (length < sizeof(dir)) {
            memcpy(dir, entry, length);
            dir[length] = '\0';
            dir_fds[n_dirs] = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dir_fds[n_dirs] >= 0)
                n_dirs++;
        }
        entry += length + (entry[length] == ':' ? 1 : 0);
    }

    r = mf_host_links_sync(links_fd, dir_fds, n_dirs);

    for (size_t i = 0; i < n_dirs; i++)
        close(dir_fds[i]);
    free(dir_fds);
    return r;
}

/**
 * mfx_ensure_user_links:
 * @user_dirs: program directories of the host, separated by colons
 * @key: a hash of @user_dirs
 * @newest: the newest modification time of the directories in @user_dirs
 *
 * Get the directory with the links to the programs in @user_dirs. @user_dirs
 * holds the program directories that the compose service has no links for,
 * like the ones in the home directory of the user. We keep one links
 * directory for every set of program directories. It is located in the
 * runtime directory of the user, because that directory is available in
 * every tree.
 *
 * To keep launches fast, we only read the program directories if one of them
 * has changed. We set the modification time of the links directory to the
 * newest modification time of its program directories. The links are up to
 * date if the two times are still equal.
 *
 * Returns: the links directory, or nullptr if the user has no runtime
 *          directory or we failed to create the links
 */
static char *
mfx_ensure_user_links(const char *user_dirs, uint64_t key, const struct timespec *newest)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    const struct timespec times[2] = {{.tv_nsec = UTIME_OMIT}, *newest};
    char *links_dir = nullptr;
    struct stat st;
    int links_fd;
    int r;

    if (runtime_dir == nullptr || runtime_dir[0] != '/')
        return nullptr;
    if (asprintf(&links_dir, "%s/" MF_HOST_USER_BIN_SUBDIR "/%016" PRIx64, runtime_dir, key) < 0)
        return nullptr;
    if (stat(links_dir, &st) == 0 && st.st_mtim.tv_sec == newest->tv_sec &&
        st.st_mtim.tv_nsec == newest->tv_nsec)
        return links_dir;

    /* create the links directory and its two parent directories, which usually exist already */
    for (int level = 2; level >= 0; level--) {
        char *end = links_dir + strlen(links_dir);

        for (int i = 0; i < level; i++) {
            do
                end--;
            while (*end != '/');
        }
        char saved = *end;
        *end = '\0';
        r = mkdir(links_dir, 0700) != 0 && errno != EEXIST ? -errno : 0;
        *end = saved;
        if (r < 0)
            goto fail;
    }

    links_fd = open(links_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (links_fd < 0) {
        r = -errno;
        goto fail;
    }
    r = mfx_sync_user_links(links_fd, user_dirs);
    /* NOTE: We do not set the current time here. If a program directory changes while
     * we read it, its modification time is newer than the time that we set on the links
     * directory, so the next launch updates the links again. */
    if (r >= 0 && futimens(links_fd, times) != 0)
        r = -errno;
    close(links_fd);
    if (r < 0)
        goto fail;

    return links_dir;

fail:
    fprintf(stderr,
            "%s: Unable to link the programs of the host in %s: %s\n",
            PROGRAM_NAME,
            links_dir,
            strerror(-r));
    free(links_dir);
    return nullptr;
}

/**
 * mfx_build_host_search_path:
 * @caller_path: the cleaned PATH of the caller
 * @host_commands: the directories for running programs of the host
 *
 * Build the PATH of an application that may run programs of the host.
 *
 * A program of the host must not run in the tree, where it would use the
 * libraries of the tree. So we only keep the directories of the caller that
 * the compose service has links for. We replace all other directories, like
 * directories in the home directory of the user, with a directory that has
 * links to their programs. The directories with links are added last, so
 * the programs of the tree are preferred.
 *
 * Returns: the search path, or nullptr if we are out of memory
 */
static char *
mfx_build_host_search_path(const char *caller_path, const MfxHostCommands *host_commands)
{
    size_t size = strlen(caller_path) + 1;
    char *system_dirs = calloc(1, size);
    char *user_dirs = calloc(1, size);
    char *links_dir = nullptr;
    char *search_path = nullptr;
    struct timespec newest = {0};
    uint64_t key = 0xcbf29ce484222325; /* FNV-1a */
    int r;

    if (system_dirs == nullptr || user_dirs == nullptr)
        goto out;

    for (const char *entry = caller_path; *entry != '\0';) {
        size_t length = strcspn(entry, ":");
        bool is_system_dir = false;
        char dir[PATH_MAX];
        struct stat st;

        for (char **s = host_commands->system_paths; s != nullptr && *s != nullptr && !is_system_dir; s++)
            is_system_dir = strlen(*s) == length && strncmp(*s, entry, length) == 0;
        if (is_system_dir) {
            mfx_search_path_add(system_dirs, entry, length);
        } else if (length < sizeof(dir)) {
            memcpy(dir, entry, length);
            dir[length] = '\0';
            if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                mfx_search_path_add(user_dirs, entry, length);
                for (size_t i = 0; i <= length; i++)
                    key = (key ^ (unsigned char) dir[i]) * 0x100000001b3;
                if (st.st_mtim.tv_sec > newest.tv_sec ||
                    (st.st_mtim.tv_sec == newest.tv_sec && st.st_mtim.tv_nsec > newest.tv_nsec))
                    newest = st.st_mtim;
            }
        }
        entry += length + (entry[length] == ':' ? 1 : 0);
    }

    if (user_dirs[0] != '\0')
        links_dir = mfx_ensure_user_links(user_dirs, key, &newest);
    if (links_dir != nullptr || user_dirs[0] == '\0')
        r = asprintf(&search_path,
                     "%s%s%s%s%s:%s",
                     MF_TREE_APP_BIN_DIR,
                     system_dirs[0] != '\0' ? ":" : "",
                     system_dirs,
                     links_dir != nullptr ? ":" : "",
                     links_dir != nullptr ? links_dir : "",
                     host_commands->directory);
    else
        /* we have no links for the user directories, so we keep them and their programs run in the tree */
        r = asprintf(&search_path,
                     "%s%s%s:%s",
                     MF_TREE_APP_BIN_DIR,
                     caller_path[0] != '\0' ? ":" : "",
                     caller_path,
                     host_commands->directory);
    if (r < 0)
        search_path = nullptr;

out:
    free(system_dirs);
    free(user_dirs);
    free(links_dir);
    return search_path;
}

/**
 * mfx_set_search_path:
 * @caller_path: the cleaned PATH of the caller
 * @search_path: the PATH of the application
 * @for_host_commands: whether the application may run programs of the host
 *
 * Set the PATH of the application. For an application that may run programs
 * of the host, we also pass on the PATH of the caller in a separate variable.
 * The host uses it as search path when it runs a program for the application.
 *
 * Returns: 0 on success, or a negative errno
 */
static int
mfx_set_search_path(const char *caller_path, const char *search_path, bool for_host_commands)
{
    if (setenv("PATH", search_path, 1) != 0)
        return -errno;
    if (for_host_commands)
        return setenv(MF_HOST_PATH_VARIABLE, caller_path, 1) != 0 ? -errno : 0;
    /* the variable may be set if we were called by a different Manifold application */
    return unsetenv(MF_HOST_PATH_VARIABLE) != 0 ? -errno : 0;
}

/**
 * mfx_execute_path:
 *
 * Execute the program at @path.
 *
 * Returns: the exit status to exit with. This function only returns if the
 *   program can not be executed.
 */
static int
mfx_execute_path(const char *path, char **args)
{
    execv(path, args);

    int saved_errno = errno;
    fprintf(stderr, "%s: Unable to execute %s: %s\n", PROGRAM_NAME, path, strerror(saved_errno));
    return saved_errno == ENOENT ? MFX_EXIT_NOT_FOUND : MFX_EXIT_CANNOT_EXECUTE;
}

/**
 * mfx_execute_command:
 *
 * Execute a program in the tree. @command is either the path of the
 * program, or its name, which we look up in the PATH of the application.
 *
 * Returns: the exit status to exit with. This function only returns if the
 *   program can not be found or executed.
 */
static int
mfx_execute_command(const char *command, char **args)
{
    const char *path_env = getenv("PATH");
    char *search = nullptr;
    char *saveptr = nullptr;

    args[0] = (char *) command;
    if (strchr(command, '/') != nullptr)
        return mfx_execute_path(command, args);

    search = strdup(path_env != nullptr ? path_env : "");
    if (search == nullptr) {
        fprintf(stderr, "%s: Out of memory\n", PROGRAM_NAME);
        return MFX_EXIT_FAILED;
    }
    for (const char *dir = strtok_r(search, ":", &saveptr); dir != nullptr;
         dir = strtok_r(nullptr, ":", &saveptr)) {
        char path[PATH_MAX];
        struct stat st;

        if (snprintf(path, sizeof(path), "%s/%s", dir, command) >= (int) sizeof(path))
            continue;
        /* like the shell, we skip an entry with the name of the program that is a directory */
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || access(path, X_OK) != 0)
            continue;
        free(search);
        return mfx_execute_path(path, args);
    }

    fprintf(stderr, "%s: There is no program %s in the component\n", PROGRAM_NAME, command);
    free(search);
    return MFX_EXIT_NOT_FOUND;
}

/**
 * main:
 */
int
main(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"socket",  required_argument, nullptr, 's'},
        {"command", required_argument, nullptr, 'c'},
        {"help",    no_argument,       nullptr, 'h'},
        {"version", no_argument,       nullptr, 'v'},
        {nullptr,   0,                 nullptr, 0  },
    };
    const char *socket_path = MF_RUN_SOCKET_PATH;
    const char *command = nullptr;
    char cwd[PATH_MAX];
    MfxLaunchData launch_data = {.mount_fd = -1};
    char *caller_path;
    char *search_path = nullptr;
    bool for_host_commands;
    int opt;

    /* the leading "+" stops parsing at the first non-option argument */
    while ((opt = getopt_long(argc, argv, "+", long_options, nullptr)) != -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 'c':
            command = optarg;
            break;
        case 'h':
            mfx_print_usage(stdout);
            return EXIT_SUCCESS;
        case 'v':
            printf(PROGRAM_NAME " " MF_VERSION "\n");
            return EXIT_SUCCESS;
        default:
            mfx_print_usage(stderr);
            return MFX_EXIT_FAILED;
        }
    }

    if (argc - optind < 1) {
        mfx_print_usage(stderr);
        return MFX_EXIT_FAILED;
    }

    /* everything after the component identifier is an argument of the application */
    const char *component_id = argv[optind];

    if (mfx_prepare_launch(socket_path, component_id, &launch_data) < 0)
        return MFX_EXIT_FAILED;

    /* build the search path before we enter the tree, as we need to read directories of the host for it */
    for_host_commands = launch_data.host_commands.directory != nullptr;
    caller_path = mfx_clean_search_path(getenv("PATH"));
    if (caller_path != nullptr && for_host_commands)
        search_path = mfx_build_host_search_path(caller_path, &launch_data.host_commands);
    else if (caller_path != nullptr && asprintf(&search_path, "%s:%s", MF_TREE_APP_BIN_DIR, caller_path) < 0)
        search_path = nullptr;
    if (search_path == nullptr) {
        fprintf(stderr, "%s: Out of memory\n", PROGRAM_NAME);
        mfx_launch_data_clear(&launch_data);
        return MFX_EXIT_FAILED;
    }

    if (getcwd(cwd, sizeof(cwd)) == nullptr)
        cwd[0] = '\0';
    if (mfx_enter_tree(launch_data.mount_fd, !mfx_have_cap_sys_admin()) < 0) {
        mfx_launch_data_clear(&launch_data);
        return MFX_EXIT_FAILED;
    }
    close(launch_data.mount_fd);
    launch_data.mount_fd = -1;

    mfx_change_directory(cwd[0] != '\0' ? cwd : nullptr);
    mfx_apply_environment(launch_data.environment);

    if (mfx_set_search_path(caller_path, search_path, for_host_commands) < 0) {
        fprintf(stderr, "%s: Out of memory\n", PROGRAM_NAME);
        mfx_launch_data_clear(&launch_data);
        return MFX_EXIT_FAILED;
    }
    free(caller_path);
    free(search_path);

    /* argv[optind] is replaced with the program name, the arguments after it are passed to the program */
    int status = mfx_execute_command(command != nullptr ? command : launch_data.executable, &argv[optind]);
    mfx_launch_data_clear(&launch_data);
    return status;
}
