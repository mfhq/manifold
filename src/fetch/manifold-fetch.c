/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <locale.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <curl/curl.h>
#include <glib.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

#include "mf-fetch-interface.h"
#include "mf-sdautoptr-private.h"
#include "mf-varlink-util.h"
#include "mff-downloader.h"

/*
 * The download helper of manifoldd. It downloads files over https and writes
 * them into file descriptors that manifoldd passes to it. The helper does not
 * open any files itself, and manifoldd verifies all data it wrote before using it.
 *
 * systemd starts one helper instance per connection to its socket, with access to
 * nothing but the network (see manifold-fetch@.service). For tests, manifoldd can
 * run the helper itself and talk to it over a socket pair instead.
 */

/* the highest number of ranges in one call, to limit the memory a misbehaving caller can make us use */
constexpr guint RANGES_MAX = 65536;

/* how much data we receive before we report progress again */
constexpr guint64 PROGRESS_STEP_BYTES = 1024 * 1024;

typedef struct {
    sd_event *event;
    const char *ca_file; /* the only certificate authority we trust, or NULL to use the system's */
} MffHelper;

/* the download progress we reported to the caller so far */
typedef struct {
    sd_varlink *link;
    gboolean notify; /* whether the caller asked for progress replies */
    guint64 reported_bytes;
} MffProgress;

/**
 * mff_parse_ranges:
 * @ret_ranges: (out) (transfer full) (element-type MffRange): the ranges
 *
 * Returns: %TRUE if every range is within @max_size
 */
static gboolean
mff_parse_ranges(sd_json_variant *variant, guint64 max_size, guint *n_ranges, GArray **ret_ranges)
{
    g_autoptr(GArray) ranges = g_array_new(FALSE, FALSE, sizeof(MffRange));

    if (!sd_json_variant_is_array(variant) || sd_json_variant_elements(variant) == 0)
        return FALSE;
    for (size_t i = 0; i < sd_json_variant_elements(variant); i++) {
        sd_json_variant *element = sd_json_variant_by_index(variant, i);
        MffRange range = {};
        static const sd_json_dispatch_field dispatch_table[] = {
            {"offset",
             SD_JSON_VARIANT_UNSIGNED, sd_json_dispatch_uint64,
             offsetof(MffRange, offset),
             SD_JSON_MANDATORY},
            {"length",
             SD_JSON_VARIANT_UNSIGNED, sd_json_dispatch_uint64,
             offsetof(MffRange, length),
             SD_JSON_MANDATORY},
            {},
        };

        if (sd_json_dispatch(element, dispatch_table, SD_JSON_LOG, &range) < 0)
            return FALSE;
        if (range.length == 0 || range.offset > max_size || range.length > max_size - range.offset)
            return FALSE;
        if (++*n_ranges > RANGES_MAX)
            return FALSE;
        g_array_append_val(ranges, range);
    }

    *ret_ranges = g_steal_pointer(&ranges);
    return TRUE;
}

/**
 * mff_parse_requests:
 *
 * Read the requests of a call, and take the file descriptors they name.
 *
 * Returns: (transfer full) (element-type MffRequest): the requests, or %NULL
 *   after replying with an error
 */
static GPtrArray *
mff_parse_requests(sd_varlink *link, sd_json_variant *parameters, int *ret_reply)
{
    g_autoptr(GPtrArray) requests = g_ptr_array_new_with_free_func((GDestroyNotify) mff_request_free);
    sd_json_variant *list = sd_json_variant_by_key(parameters, "requests");
    guint n_ranges = 0;

    if (list == NULL || !sd_json_variant_is_array(list) || sd_json_variant_elements(list) == 0 ||
        sd_json_variant_elements(list) > MF_FETCH_REQUESTS_MAX) {
        *ret_reply = sd_varlink_error_invalid_parameter_name(link, "requests");
        return NULL;
    }

    for (size_t i = 0; i < sd_json_variant_elements(list); i++) {
        struct {
            const char *url;
            unsigned int fd_index;
            guint64 max_size;
            sd_json_variant *ranges;
        } p = {};
        static const sd_json_dispatch_field dispatch_table[] = {
            {"url",
             SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
             offsetof(typeof(p), url),
             SD_JSON_MANDATORY},
            {"fileDescriptor",
             _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint,
             offsetof(typeof(p), fd_index),
             SD_JSON_MANDATORY},
            {"maxSize",
             SD_JSON_VARIANT_UNSIGNED, sd_json_dispatch_uint64,
             offsetof(typeof(p), max_size),
             SD_JSON_MANDATORY},
            {"ranges",
             _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_variant_noref,
             offsetof(typeof(p), ranges),
             0},
            {},
        };
        g_autoptr(GArray) ranges = NULL;
        int fd;

        if (sd_json_dispatch(sd_json_variant_by_index(list, i), dispatch_table, SD_JSON_LOG, &p) < 0) {
            *ret_reply = sd_varlink_error_invalid_parameter_name(link, "requests");
            return NULL;
        }
        /* we only download via https */
        if (!g_str_has_prefix(p.url, "https://")) {
            *ret_reply = sd_varlink_error_invalid_parameter_name(link, "url");
            return NULL;
        }
        if (p.ranges != NULL && !sd_json_variant_is_null(p.ranges) &&
            !mff_parse_ranges(p.ranges, p.max_size, &n_ranges, &ranges)) {
            *ret_reply = sd_varlink_error_invalid_parameter_name(link, "ranges");
            return NULL;
        }
        fd = sd_varlink_take_fd(link, p.fd_index);
        if (fd < 0) {
            *ret_reply = sd_varlink_error_invalid_parameter_name(link, "fileDescriptor");
            return NULL;
        }

        g_ptr_array_add(requests, mff_request_new(p.url, fd, p.max_size, g_steal_pointer(&ranges)));
    }

    return g_steal_pointer(&requests);
}

/**
 * mff_on_progress:
 *
 * Tell the caller how much data we have received, and stop the download if
 * the caller has closed the connection. The event loop does not run during a
 * download, so we have to flush the reply ourselves.
 */
static gboolean
mff_on_progress(guint64 received, gpointer user_data)
{
    MffProgress *progress = user_data;
    struct pollfd pfd = {.fd = sd_varlink_get_fd(progress->link), .events = POLLIN};

    /* there is nobody to download for anymore if the caller has closed the connection */
    if (pfd.fd < 0 || (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLHUP | POLLERR)) != 0))
        return FALSE;

    if (!progress->notify || received - progress->reported_bytes < PROGRESS_STEP_BYTES)
        return TRUE;
    progress->reported_bytes = received;
    if (sd_varlink_notifybo(progress->link, SD_JSON_BUILD_PAIR_UNSIGNED("received", received)) < 0 ||
        sd_varlink_flush(progress->link) < 0)
        return FALSE;
    return TRUE;
}

/**
 * mff_method_fetch:
 *
 * Download the requests of a call. This blocks until all downloads are done,
 * and replies with one result for each request.
 */
static int
mff_method_fetch(sd_varlink *link,
                 sd_json_variant *parameters,
                 sd_varlink_method_flags_t flags,
                 void *userdata)
{
    MffHelper *helper = userdata;
    MffProgress progress = {.link = link, .notify = (flags & SD_VARLINK_METHOD_MORE) != 0};
    g_autoptr(GPtrArray) requests = NULL;
    g_autoptr(sd_json_variant) results = NULL;
    g_autoptr(GError) error = NULL;
    guint64 received = 0;
    int r = 0;

    requests = mff_parse_requests(link, parameters, &r);
    if (requests == NULL)
        return r;

    if (!mff_download(requests, helper->ca_file, mff_on_progress, &progress, &error)) {
        g_warning("%s", error->message);
        return sd_varlink_error(link, SD_VARLINK_ERROR_SYSTEM, NULL);
    }

    for (guint i = 0; i < requests->len; i++) {
        MffRequest *request = g_ptr_array_index(requests, i);
        gboolean failed = request->failure != MFF_FAILURE_KIND_NONE;

        received += request->size;
        r = sd_json_variant_append_arraybo(
            &results,
            SD_JSON_BUILD_PAIR_UNSIGNED("size", request->size),
            SD_JSON_BUILD_PAIR_CONDITION(failed,
                                         "failure",
                                         SD_JSON_BUILD_STRING(mff_failure_kind_to_string(request->failure))),
            SD_JSON_BUILD_PAIR_CONDITION(request->http_status > 0,
                                         "httpStatus",
                                         SD_JSON_BUILD_INTEGER(request->http_status)),
            SD_JSON_BUILD_PAIR_CONDITION(failed && request->message != NULL,
                                         "message",
                                         SD_JSON_BUILD_STRING(request->message)));
        if (r < 0)
            return r;
        if (failed)
            g_info("Unable to download %s: %s", request->url, request->message);
    }

    return sd_varlink_replybo(link,
                              SD_JSON_BUILD_PAIR_UNSIGNED("received", received),
                              SD_JSON_BUILD_PAIR_VARIANT("results", results));
}

/**
 * mff_on_disconnect:
 *
 * The helper serves a single connection, and exits when it is closed.
 */
static void
mff_on_disconnect(sd_varlink_server *server, sd_varlink *link, void *userdata)
{
    MffHelper *helper = userdata;

    sd_event_exit(helper->event, 0);
}

/**
 * mff_run:
 * @connection_fd: the connection to serve, or -1 for the one systemd gave us
 */
static int
mff_run(int connection_fd, const char *ca_file)
{
    g_autoptr(sd_event) event = NULL;
    g_autoptr(sd_varlink_server) server = NULL;
    g_autoptr(GError) error = NULL;
    MffHelper helper = {.ca_file = ca_file};
    int r;

    event = mf_event_new_default(&error);
    if (event == NULL) {
        g_critical("%s", error->message);
        return EXIT_FAILURE;
    }
    helper.event = event;

    server = mf_varlink_server_new(event,
                                   "manifold-fetch",
#if HAVE_SD_VARLINK_SERVER_FD_PASSING
                                   SD_VARLINK_SERVER_ALLOW_FD_PASSING_INPUT,
#else
                                   0,
#endif
                                   &vl_interface_org_freedesktop_manifold_fetch,
                                   &helper,
                                   &error);
    if (server == NULL) {
        g_critical("%s", error->message);
        return EXIT_FAILURE;
    }
    r = sd_varlink_server_bind_method(server, MF_FETCH_METHOD_FETCH, mff_method_fetch);
    if (r < 0) {
        g_critical("Unable to bind the Fetch method: %s", g_strerror(-r));
        return EXIT_FAILURE;
    }
    sd_varlink_server_bind_disconnect(server, mff_on_disconnect);

#if !HAVE_SD_VARLINK_SERVER_FD_PASSING
    /* systemd < 258 can only allow receiving file descriptors per connection, and it does
     * not run the connect callback for connections it did not accept itself. So we add the
     * connection systemd accepted for us ourselves, like the one of --connection-fd. */
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
        /* systemd passes us the connection it accepted, see manifold-fetch.socket */
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
    g_autofree char *ca_file = NULL;
    int connection_fd = -1;
    gboolean verbose = FALSE;
    gboolean show_version = FALSE;
    int ret;
    const GOptionEntry entries[] = {
        {"connection-fd",
         0,                  0,
         G_OPTION_ARG_INT,                         &connection_fd,
         "Serve the connection on file descriptor FD",                                         "FD"  },
        {"ca-file",
         0,                  0,
         G_OPTION_ARG_FILENAME,                    &ca_file,
         "Only trust the certificate authority in FILE",                                       "FILE"},
        {"verbose",       0, 0, G_OPTION_ARG_NONE, &verbose,       "Show more information",    NULL  },
        {"version",       0, 0, G_OPTION_ARG_NONE, &show_version,  "Show the program version", NULL  },
        G_OPTION_ENTRY_NULL,
    };

    setlocale(LC_ALL, "");

    context = g_option_context_new("- Manifold download helper");
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
        g_print("manifold-fetch %s\n", MF_VERSION);
        return EXIT_SUCCESS;
    }
    if (verbose)
        g_log_set_debug_enabled(TRUE);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        g_critical("Unable to set up libcurl");
        return EXIT_FAILURE;
    }
    ret = mff_run(connection_fd, ca_file);
    curl_global_cleanup();

    return ret;
}
