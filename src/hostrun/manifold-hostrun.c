/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <glib.h>
#include <glib-unix.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

#include "mf-constants-private.h"
#include "mf-host-interface.h"
#include "mf-sdautoptr-private.h"
#include "mf-varlink-util.h"

/*
 * The host command runner. An application that has the permission to do so
 * may ask us to run a program that belongs to the host (which it can't do itself,
 * because we confine the app in its container).
 * We run the program as the user, in the session of the user, with the standard input
 * and output of the caller.
 *
 * systemd starts one runner for every connection to our socket in the session
 * of the user (see manifold-host@.service), and a caller opens one connection
 * for every program. This way, every program runs in a systemd unit of its
 * own. We exit when the caller closes the connection, and systemd then stops
 * all remaining processes of the program.
 */

/* the name of the permission that an application needs to call us */
constexpr char HOST_COMMANDS_PERMISSION[] = "host-commands";

/* the maximum number of file descriptors that we pass on to a program */
constexpr size_t FDS_MAX = 64;

/**
 * MfhRunner:
 *
 * The state of the runner, which runs one program at a time.
 */
typedef struct {
    sd_event *event;
    sd_varlink *link;               /* the connection to reply on when the program ends, or NULL */
    pid_t pid;                      /* process ID of the program, or 0 if no program is running */
    sd_event_source *child_source;  /* event source that notifies us when the program ends */
    sd_event_source *signal_source; /* the pipe on which the caller sends signals for the program */
} MfhRunner;

/**
 * MfhCaller:
 *
 * The process that asked us to run a program.
 */
typedef struct {
    char *component_id;       /* the application the process belongs to, or NULL for a process of the host */
    gboolean has_permission;  /* whether the application may run programs of the host */
    char **managed_variables; /* names of the environment variables that are set for the tree */
} MfhCaller;

/**
 * mfh_caller_clear:
 */
static void
mfh_caller_clear(MfhCaller *caller)
{
    g_clear_pointer(&caller->component_id, g_free);
    g_clear_pointer(&caller->managed_variables, g_strfreev);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfhCaller, mfh_caller_clear)

/**
 * mfh_close_fd:
 */
static void
mfh_close_fd(int *fd)
{
    close(*fd);
}

/**
 * mfh_identify_caller:
 * @link: the connection of the caller
 * @caller: (out): the caller
 *
 * Find out which application the calling process belongs to.
 *
 * Every tree contains the environment record of the application that the
 * tree was assembled for, and the application can not change the record.
 * We read the record via the root directory of the calling process. If the
 * root directory has no record, the caller is a process of the host, which
 * is allowed to run any program anyway.
 *
 * Returns: 0 on success, or a negative errno if we can not identify the caller
 */
static int
mfh_identify_caller(sd_varlink *link, MfhCaller *caller)
{
    g_autofree char *path = NULL;
    g_autofree char *data = NULL;
    g_autoptr(sd_json_variant) record = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GStrvBuilder) variables = g_strv_builder_new();
    sd_json_variant *component_id;
    sd_json_variant *permissions;
    sd_json_variant *environment;
    gboolean have_record;
    pid_t pid;
    int pidfd;

    int r = sd_varlink_get_peer_pid(link, &pid);
    if (r < 0)
        return r;
    pidfd = sd_varlink_get_peer_pidfd(link);
    if (pidfd < 0)
        return pidfd;

    path = g_strdup_printf("/proc/%" G_PID_FORMAT "/root/%s", pid, MF_TREE_ENVIRONMENT_RECORD);
    have_record = g_file_get_contents(path, &data, NULL, &error);
    if (!have_record && !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        return -EACCES;
    /* check that the caller still exists, otherwise its process ID may have been reused */
    if (syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0) != 0)
        return -ESRCH;
    if (!have_record)
        return 0;

    if (sd_json_parse(data, 0, &record, NULL, NULL) < 0 || !sd_json_variant_is_object(record))
        return -EBADMSG;
    component_id = sd_json_variant_by_key(record, "componentId");
    if (component_id == NULL || !sd_json_variant_is_string(component_id))
        return -EBADMSG;
    caller->component_id = g_strdup(sd_json_variant_string(component_id));

    permissions = sd_json_variant_by_key(record, "permissions");
    if (permissions != NULL && sd_json_variant_is_array(permissions)) {
        for (size_t i = 0; i < sd_json_variant_elements(permissions); i++) {
            sd_json_variant *permission = sd_json_variant_by_index(permissions, i);

            if (sd_json_variant_is_string(permission) &&
                g_str_equal(sd_json_variant_string(permission), HOST_COMMANDS_PERMISSION))
                caller->has_permission = TRUE;
        }
    }

    /* the elements of a JSON object alternate between keys and values, so every second element is a key */
    environment = sd_json_variant_by_key(record, "environment");
    if (environment != NULL && sd_json_variant_is_object(environment)) {
        for (size_t i = 0; i < sd_json_variant_elements(environment); i += 2)
            g_strv_builder_add(variables, sd_json_variant_string(sd_json_variant_by_index(environment, i)));
    }
    caller->managed_variables = g_strv_builder_end(variables);

    return 0;
}

/**
 * mfh_search_path_add:
 * @search_path: (inout): directories, separated by colons
 * @seen: (inout): the set of directories that are in @search_path
 * @dirs: directories to add, separated by colons
 * @skip_tree_dirs: whether to skip the directories that only exist in a tree
 *
 * Add directories to a search path. A directory that is in the search path
 * already is not added again.
 */
static void
mfh_search_path_add(GString *search_path, GHashTable *seen, const char *dirs, gboolean skip_tree_dirs)
{
    g_autofree char *user_links_prefix = g_build_filename(g_get_user_runtime_dir(),
                                                          MF_HOST_USER_BIN_SUBDIR,
                                                          "/",
                                                          NULL);
    g_auto(GStrv) entries = g_strsplit(dirs, ":", -1);

    for (char **dir = entries; *dir != NULL; dir++) {
        if (**dir == '\0' || g_hash_table_contains(seen, *dir))
            continue;
        if (skip_tree_dirs && (g_str_equal(*dir, MF_TREE_APP_BIN_DIR) || g_str_equal(*dir, MF_HOST_BIN_DIR) ||
                               g_str_has_prefix(*dir, user_links_prefix)))
            continue;
        if (search_path->len > 0)
            g_string_append_c(search_path, ':');
        g_string_append(search_path, *dir);
        g_hash_table_add(seen, g_strdup(*dir));
    }
}

/**
 * mfh_build_environment:
 * @caller: the caller
 * @caller_environment: (nullable): the environment of the caller, as NAME=VALUE entries
 *
 * Build the environment of the program.
 *
 * The program gets the environment of the session of the user, with the
 * variables of the caller set on top. Variables that are set for the tree of
 * the application, like the paths of its graphics drivers, are left out, as
 * their values are wrong on the host.
 *
 * In the PATH of an application, we skip the directories that only exist in
 * the tree (the program directory of the application and the directories of
 * the trampoline links), and append the PATH of the host from
 * %MF_HOST_PATH_VARIABLE.
 *
 * Returns: (transfer full): the environment
 */
static char **
mfh_build_environment(const MfhCaller *caller, char **caller_environment)
{
    g_auto(GStrv) environment = g_get_environ();
    g_autoptr(GString) search_path = g_string_new(NULL);
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean is_application = caller->component_id != NULL;
    const char *caller_path = NULL;
    const char *host_path = NULL;

    for (char **variable = caller_environment; variable != NULL && *variable != NULL; variable++) {
        g_autofree char *name = NULL;
        const char *value = strchr(*variable, '=');

        if (value == NULL || value == *variable)
            continue;
        name = g_strndup(*variable, value - *variable);
        value++;

        if (g_str_equal(name, "PATH"))
            caller_path = value;
        else if (g_str_equal(name, MF_HOST_PATH_VARIABLE))
            host_path = value;
        else if (caller->managed_variables == NULL ||
                 !g_strv_contains((const char *const *) caller->managed_variables, name))
            environment = g_environ_setenv(environment, name, value, TRUE);
    }

    if (caller_path != NULL)
        mfh_search_path_add(search_path, seen, caller_path, is_application);
    if (is_application && host_path != NULL)
        mfh_search_path_add(search_path, seen, host_path, FALSE);
    /* if the caller has sent no PATH, the program keeps the PATH of our own environment */
    if (search_path->len > 0)
        environment = g_environ_setenv(environment, "PATH", search_path->str, TRUE);
    environment = g_environ_unsetenv(environment, MF_HOST_PATH_VARIABLE);

    return g_steal_pointer(&environment);
}

/**
 * mfh_is_same_directory:
 *
 * Returns: %TRUE if @path is the directory that @dir_fd refers to
 */
static gboolean
mfh_is_same_directory(const char *path, int dir_fd)
{
    struct stat path_st;
    struct stat fd_st;

    if (stat(path, &path_st) != 0 || fstat(dir_fd, &fd_st) != 0)
        return FALSE;
    return S_ISDIR(path_st.st_mode) && path_st.st_dev == fd_st.st_dev && path_st.st_ino == fd_st.st_ino;
}

/**
 * mfh_runner_forget_program:
 *
 * Reset the state of the runner. This is called once the program has ended
 * and the reply was sent to the caller.
 */
static void
mfh_runner_forget_program(MfhRunner *runner)
{
    g_clear_pointer(&runner->child_source, sd_event_source_disable_unref);
    g_clear_pointer(&runner->signal_source, sd_event_source_disable_unref);
    g_clear_pointer(&runner->link, sd_varlink_unref);
    runner->pid = 0;
}

/**
 * mfh_on_program_ended:
 *
 * Called when the program has ended. We reply to the caller with the exit
 * status of the program, or with the signal that terminated it.
 */
static int
mfh_on_program_ended(sd_event_source *source, const siginfo_t *info, void *userdata)
{
    MfhRunner *runner = userdata;

    /* NOTE: The reply functions return a negative errno if the caller is gone, which is fine. */
    if (info->si_code == CLD_EXITED)
        sd_varlink_replybo(runner->link, SD_JSON_BUILD_PAIR_INTEGER("exitStatus", info->si_status));
    else
        sd_varlink_replybo(runner->link, SD_JSON_BUILD_PAIR_INTEGER("signal", info->si_status));
    mfh_runner_forget_program(runner);

    return 0;
}

/**
 * mfh_on_signal_requested:
 *
 * Called when the caller wrote to its signal pipe. Every byte is a signal
 * number. We send the signal to the process group of the program.
 */
static int
mfh_on_signal_requested(sd_event_source *source, int fd, uint32_t revents, void *userdata)
{
    MfhRunner *runner = userdata;
    uint8_t signals[64];
    ssize_t n_signals = read(fd, signals, sizeof(signals));

    if (n_signals < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    /* the caller closed the pipe, so it has no more signals for us */
    if (n_signals <= 0) {
        g_clear_pointer(&runner->signal_source, sd_event_source_disable_unref);
        return 0;
    }

    for (ssize_t i = 0; i < n_signals; i++) {
        if (signals[i] > 0 && signals[i] < NSIG)
            kill(-runner->pid, signals[i]);
    }
    return 0;
}

/**
 * mfh_setup_program:
 *
 * Called in the new process, before it executes the program.
 */
static void
mfh_setup_program(gpointer user_data)
{
    sigset_t no_signals;

    /* create a new process group, so a signal reaches the program and all processes that it started */
    setpgid(0, 0);
    /* we block the signals that our event loop handles, and the program must not inherit the signal mask */
    sigemptyset(&no_signals);
    sigprocmask(SIG_SETMASK, &no_signals, NULL);
}

/**
 * mfh_watch_program:
 * @signal_fd: (transfer full): the pipe on which the caller sends signals, or -1
 *
 * Watch the program we started, and the signal pipe of the caller.
 *
 * Returns: 0 on success, or a negative errno
 */
static int
mfh_watch_program(MfhRunner *runner, int signal_fd)
{
    g_autofd int owned_signal_fd = signal_fd;

    int r = sd_event_add_child(runner->event,
                               &runner->child_source,
                               runner->pid,
                               WEXITED,
                               mfh_on_program_ended,
                               runner);
    if (r < 0 || owned_signal_fd < 0)
        return r;

    r = fcntl(owned_signal_fd, F_SETFL, O_NONBLOCK) != 0 ? -errno : 0;
    if (r >= 0)
        r = sd_event_add_io(runner->event,
                            &runner->signal_source,
                            owned_signal_fd,
                            EPOLLIN,
                            mfh_on_signal_requested,
                            runner);
    if (r >= 0)
        r = sd_event_source_set_io_fd_own(runner->signal_source, TRUE);
    if (r >= 0)
        g_steal_fd(&owned_signal_fd);
    return r;
}

/**
 * mfh_reply_spawn_error:
 *
 * Reply to the caller with the error that prevented the program from starting.
 */
static int
mfh_reply_spawn_error(sd_varlink *link,
                      const char *program,
                      const char *working_directory,
                      const GError *error)
{
    if (g_error_matches(error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
        return sd_varlink_errorbo(link,
                                  MF_HOST_ERROR_NO_SUCH_PROGRAM_ID,
                                  SD_JSON_BUILD_PAIR_STRING("program", program));
    if (g_error_matches(error, G_SPAWN_ERROR, G_SPAWN_ERROR_CHDIR))
        return sd_varlink_errorbo(link,
                                  MF_HOST_ERROR_NO_SUCH_WORKING_DIRECTORY_ID,
                                  SD_JSON_BUILD_PAIR_STRING("path", working_directory));
    return sd_varlink_errorbo(link,
                              MF_HOST_ERROR_SPAWN_FAILED_ID,
                              SD_JSON_BUILD_PAIR_STRING("message", error->message));
}

typedef struct {
    char **argv;
    const char *working_directory;
    char **environment;
    sd_json_variant *fd_indices;
    guint working_directory_fd_index;
    sd_json_variant *signal_fd_index;
} MfhRunParameters;

/**
 * mfh_run_parameters_clear:
 */
static void
mfh_run_parameters_clear(MfhRunParameters *p)
{
    g_clear_pointer(&p->argv, g_strfreev);
    g_clear_pointer(&p->environment, g_strfreev);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfhRunParameters, mfh_run_parameters_clear)

/**
 * mfh_method_run:
 */
static int
mfh_method_run(sd_varlink *link, sd_json_variant *parameters, sd_varlink_method_flags_t flags, void *userdata)
{
    static const sd_json_dispatch_field dispatch_table[] = {
        {"argv",
         SD_JSON_VARIANT_ARRAY, sd_json_dispatch_strv,
         offsetof(MfhRunParameters, argv),
         SD_JSON_MANDATORY},
        {"workingDirectory",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(MfhRunParameters, working_directory),
         SD_JSON_MANDATORY},
        {"environment",
         SD_JSON_VARIANT_ARRAY, sd_json_dispatch_strv,
         offsetof(MfhRunParameters, environment),
         SD_JSON_MANDATORY},
        {"fileDescriptors",
         SD_JSON_VARIANT_ARRAY, sd_json_dispatch_variant_noref,
         offsetof(MfhRunParameters, fd_indices),
         SD_JSON_MANDATORY},
        {"workingDirectoryFileDescriptor",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint,
         offsetof(MfhRunParameters, working_directory_fd_index),
         SD_JSON_MANDATORY},
        {"signalFileDescriptor",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_variant_noref,
         offsetof(MfhRunParameters, signal_fd_index),
         SD_JSON_NULLABLE},
        {},
    };
    MfhRunner *runner = userdata;
    g_auto(MfhRunParameters) p = {0};
    g_auto(MfhCaller) caller = {0};
    g_autoptr(GArray) fds = g_array_new(FALSE, FALSE, sizeof(int));
    g_autoptr(GArray) target_fds = g_array_new(FALSE, FALSE, sizeof(int));
    g_auto(GStrv) environment = NULL;
    g_autoptr(GError) error = NULL;
    g_autofd int working_directory_fd = -1;
    g_autofd int signal_fd = -1;
    int stdio_fds[3] = {-1, -1, -1};
    GSpawnFlags spawn_flags = G_SPAWN_SEARCH_PATH_FROM_ENVP | G_SPAWN_DO_NOT_REAP_CHILD;
    size_t n_fds;

    /* close our copies of the file descriptors once the program was started */
    g_array_set_clear_func(fds, (GDestroyNotify) mfh_close_fd);

    int r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;
    if (runner->pid != 0)
        return sd_varlink_error_errno(link, -EBUSY);
    if (p.argv[0] == NULL || p.argv[0][0] == '\0')
        return sd_varlink_error_invalid_parameter_name(link, "argv");
    if (p.working_directory[0] != '/')
        return sd_varlink_error_invalid_parameter_name(link, "workingDirectory");
    n_fds = sd_json_variant_elements(p.fd_indices);
    if (n_fds > FDS_MAX)
        return sd_varlink_error_invalid_parameter_name(link, "fileDescriptors");

    r = mfh_identify_caller(link, &caller);
    if (r < 0) {
        g_warning("Unable to identify the process calling us: %s", g_strerror(-r));
        return sd_varlink_error(link, SD_VARLINK_ERROR_PERMISSION_DENIED, NULL);
    }
    if (caller.component_id != NULL && !caller.has_permission) {
        g_info("%s asked to run %s, but may not run programs of the host", caller.component_id, p.argv[0]);
        return sd_varlink_errorbo(link,
                                  MF_HOST_ERROR_NOT_PERMITTED_ID,
                                  SD_JSON_BUILD_PAIR_STRING("componentId", caller.component_id));
    }

    for (size_t i = 0; i < n_fds; i++) {
        sd_json_variant *index = sd_json_variant_by_index(p.fd_indices, i);
        int target = (int) i;
        int fd;

        if (!sd_json_variant_is_unsigned(index))
            return sd_varlink_error_invalid_parameter_name(link, "fileDescriptors");
        /* NOTE: We must not keep a file descriptor of the caller open after the program
         * has received it, otherwise a pipe would stay open after the program has closed
         * its end. So we take every file descriptor from the connection, and duplicate it
         * if the caller lists the same index more than once. */
        fd = -1;
        for (size_t j = 0; j < i && fd < 0; j++) {
            if (sd_json_variant_unsigned(sd_json_variant_by_index(p.fd_indices, j)) ==
                sd_json_variant_unsigned(index))
                fd = fcntl(g_array_index(fds, int, j), F_DUPFD_CLOEXEC, 3);
        }
        if (fd < 0)
            fd = sd_varlink_take_fd(link, sd_json_variant_unsigned(index));
        if (fd < 0)
            return sd_varlink_error_invalid_parameter_name(link, "fileDescriptors");
        g_array_append_val(fds, fd);
        if (i < G_N_ELEMENTS(stdio_fds))
            stdio_fds[i] = fd;
        else
            g_array_append_val(target_fds, target);
    }
    working_directory_fd = sd_varlink_take_fd(link, p.working_directory_fd_index);
    if (working_directory_fd < 0)
        return sd_varlink_error_invalid_parameter_name(link, "workingDirectoryFileDescriptor");
    if (p.signal_fd_index != NULL && !sd_json_variant_is_null(p.signal_fd_index)) {
        if (sd_json_variant_is_unsigned(p.signal_fd_index))
            signal_fd = sd_varlink_take_fd(link, sd_json_variant_unsigned(p.signal_fd_index));
        if (signal_fd < 0)
            return sd_varlink_error_invalid_parameter_name(link, "signalFileDescriptor");
    }

    /* the working directory may only exist in the tree of the caller, while the host has a
     * different directory at the same path. The program must not run in that directory. */
    if (!mfh_is_same_directory(p.working_directory, working_directory_fd))
        return sd_varlink_errorbo(link,
                                  MF_HOST_ERROR_NO_SUCH_WORKING_DIRECTORY_ID,
                                  SD_JSON_BUILD_PAIR_STRING("path", p.working_directory));

    environment = mfh_build_environment(&caller, p.environment);
    g_debug("%s runs %s",
            caller.component_id != NULL ? caller.component_id : "A process of the host",
            p.argv[0]);

    /* if the caller sent no standard output or error, we use /dev/null, so the program does not inherit
     * our own standard output and error */
    if (stdio_fds[1] < 0)
        spawn_flags |= G_SPAWN_STDOUT_TO_DEV_NULL;
    if (stdio_fds[2] < 0)
        spawn_flags |= G_SPAWN_STDERR_TO_DEV_NULL;
    if (!g_spawn_async_with_pipes_and_fds(
            p.working_directory,
            (const char *const *) p.argv,
            (const char *const *) environment,
            spawn_flags,
            mfh_setup_program,
            NULL,
            stdio_fds[0],
            stdio_fds[1],
            stdio_fds[2],
            n_fds > G_N_ELEMENTS(stdio_fds) ? &g_array_index(fds, int, G_N_ELEMENTS(stdio_fds)) : NULL,
            (const int *) target_fds->data,
            target_fds->len,
            &runner->pid,
            NULL,
            NULL,
            NULL,
            &error)) {
        runner->pid = 0;
        return mfh_reply_spawn_error(link, p.argv[0], p.working_directory, error);
    }

    runner->link = sd_varlink_ref(link);
    r = mfh_watch_program(runner, g_steal_fd(&signal_fd));
    if (r < 0) {
        g_warning("Unable to watch the program we started: %s", g_strerror(-r));
        kill(-runner->pid, SIGKILL);
        mfh_runner_forget_program(runner);
        return sd_varlink_error_errno(link, r);
    }

    if ((flags & SD_VARLINK_METHOD_MORE) != 0)
        return sd_varlink_notifybo(link, SD_JSON_BUILD_PAIR_INTEGER("pid", runner->pid));
    return 0;
}

/**
 * mfh_on_disconnect:
 *
 * Called when the caller has closed the connection. We only serve a single
 * connection, so we exit.
 */
static void
mfh_on_disconnect(sd_varlink_server *server, sd_varlink *link, void *userdata)
{
    MfhRunner *runner = userdata;

    sd_event_exit(runner->event, 0);
}

/**
 * mfh_run:
 * @connection_fd: the connection to serve, or -1 to serve the connection that systemd passed to us
 *
 * Serve one connection, until the caller closes it.
 *
 * Returns: the exit status of the runner
 */
static int
mfh_run(int connection_fd)
{
    g_autoptr(sd_event) event = NULL;
    g_autoptr(sd_varlink_server) server = NULL;
    g_autoptr(GError) error = NULL;
    MfhRunner runner = {0};
    sigset_t child_signal;
    int r;

    /* sd-event can only watch child processes if SIGCHLD is blocked */
    sigemptyset(&child_signal);
    sigaddset(&child_signal, SIGCHLD);
    sigprocmask(SIG_BLOCK, &child_signal, NULL);

    event = mf_event_new_default(&error);
    if (event == NULL) {
        g_critical("%s", error->message);
        return EXIT_FAILURE;
    }
    runner.event = event;

    server = mf_varlink_server_new(event,
                                   "manifold-hostrun",
#if HAVE_SD_VARLINK_SERVER_FD_PASSING
                                   SD_VARLINK_SERVER_ALLOW_FD_PASSING_INPUT,
#else
                                   0,
#endif
                                   &vl_interface_org_freedesktop_manifold_host,
                                   &runner,
                                   &error);
    if (server == NULL) {
        g_critical("%s", error->message);
        return EXIT_FAILURE;
    }
    r = sd_varlink_server_bind_method(server, MF_HOST_METHOD_RUN, mfh_method_run);
    if (r < 0) {
        g_critical("Unable to bind the Run method: %s", g_strerror(-r));
        return EXIT_FAILURE;
    }
    sd_varlink_server_bind_disconnect(server, mfh_on_disconnect);

#if !HAVE_SD_VARLINK_SERVER_FD_PASSING
    /* systemd < 258 can only allow receiving file descriptors per connection, and
     * sd-varlink does not call the connect callback for a connection that it did not
     * accept itself. So we add the connection that systemd has accepted for us to the
     * server ourselves, like we do with a connection from --connection-fd. */
    if (connection_fd < 0 && sd_listen_fds(TRUE) == 1 &&
        sd_is_socket(SD_LISTEN_FDS_START, AF_UNIX, SOCK_STREAM, 0) > 0)
        connection_fd = SD_LISTEN_FDS_START;
#endif

    if (connection_fd >= 0) {
        sd_varlink *link = NULL;

        r = sd_varlink_server_add_connection(server, connection_fd, &link);
#if !HAVE_SD_VARLINK_SERVER_FD_PASSING
        if (r >= 0)
            r = sd_varlink_set_allow_fd_passing_input(link, true);
#endif
    } else {
        /* systemd passes us the connection that it has accepted, see manifold-host.socket */
        r = sd_varlink_server_listen_auto(server);
        if (r == 0) {
            g_critical("We were not started for a connection, see --connection-fd");
            return EXIT_FAILURE;
        }
    }
    if (r < 0) {
        g_critical("Unable to serve the connection: %s", g_strerror(-r));
        return EXIT_FAILURE;
    }

    r = mf_event_run(event);

    /* The caller has closed the connection, so we terminate the program. systemd stops
     * all remaining processes of our unit when we exit, but we may also run without systemd. */
    if (runner.pid != 0) {
        kill(-runner.pid, SIGTERM);
        kill(-runner.pid, SIGCONT);
    }
    mfh_runner_forget_program(&runner);

    return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

/**
 * main:
 */
int
main(int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    int connection_fd = -1;
    gboolean verbose = FALSE;
    gboolean show_version = FALSE;
    const GOptionEntry entries[] = {
        {"connection-fd",
         0,                  0,
         G_OPTION_ARG_INT,                         &connection_fd,
         "Serve the connection on file descriptor FD",                                         "FD"},
        {"verbose",       0, 0, G_OPTION_ARG_NONE, &verbose,       "Show more information",    NULL},
        {"version",       0, 0, G_OPTION_ARG_NONE, &show_version,  "Show the program version", NULL},
        G_OPTION_ENTRY_NULL,
    };

    setlocale(LC_ALL, "");

    context = g_option_context_new("- Manifold host command runner");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message);
        return EXIT_FAILURE;
    }
    if (argc > 1) {
        g_printerr("Unexpected argument: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (show_version) {
        g_print("manifold-hostrun %s\n", MF_VERSION);
        return EXIT_SUCCESS;
    }
    if (verbose)
        g_log_set_debug_enabled(TRUE);

    return mfh_run(connection_fd);
}
