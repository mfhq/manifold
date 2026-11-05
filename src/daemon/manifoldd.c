/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <locale.h>
#include <glib.h>
#include <glib/gi18n-lib.h>

#include <sys/stat.h>
#include <systemd/sd-event.h>

#include "mf-sdautoptr-private.h"
#include "mf-varlink-util.h"
#include "mfd-service.h"

static int
mfd_run_service(const char *socket_path, const char *fetch_helper, const char *fetch_ca_file)
{
    g_autoptr(sd_event) event = NULL;
    g_autoptr(MfdService) service = NULL;
    g_autoptr(GError) error = NULL;

    event = mf_event_new_default(&error);
    if (event == NULL) {
        g_critical("%s", error->message);
        return EXIT_FAILURE;
    }

    service = mfd_service_new(event, &error);
    if (service == NULL) {
        g_critical("%s", error->message);
        return EXIT_FAILURE;
    }

    if (fetch_helper != NULL)
        mfd_service_set_fetch_helper(service, fetch_helper, fetch_ca_file);

    if (!mfd_service_listen(service, socket_path, &error)) {
        g_critical("%s", error->message);
        return EXIT_FAILURE;
    }

    return mf_event_run(event) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

int
main(int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *socket_path = NULL;
    g_autofree char *fetch_helper = NULL;
    g_autofree char *fetch_ca_file = NULL;
    gboolean verbose = FALSE;
    gboolean show_version = FALSE;
    const GOptionEntry entries[] = {
        {"socket",        0, 0, G_OPTION_ARG_FILENAME, &socket_path,   "Listen on the socket at PATH", "PATH"},
        {"fetch-helper",
         0,                  0,
         G_OPTION_ARG_FILENAME,                        &fetch_helper,
         "Run the download helper at PATH ourselves",                                                  "PATH"},
        {"fetch-ca-file",
         0,                  0,
         G_OPTION_ARG_FILENAME,                        &fetch_ca_file,
         "Make the download helper trust only the certificate authority in FILE",                      "FILE"},
        {"verbose",       0, 0, G_OPTION_ARG_NONE,     &verbose,       "Show more information",        NULL  },
        {"version",       0, 0, G_OPTION_ARG_NONE,     &show_version,  "Show the program version",     NULL  },
        G_OPTION_ENTRY_NULL,
    };

    /* bind locale */
    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    context = g_option_context_new("- Manifold management daemon");
    g_option_context_set_description(context, "The default socket is " MF_SOCKET_PATH);
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
        g_print("manifoldd %s\n", MF_VERSION);
        return EXIT_SUCCESS;
    }

    if (verbose)
        g_log_set_debug_enabled(TRUE);

    /* make the files that we create world-readable, regardless of the inherited umask */
    umask(022);

    return mfd_run_service(socket_path, fetch_helper, fetch_ca_file);
}
