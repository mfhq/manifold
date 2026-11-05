/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-remote.h"

#include <glib/gi18n-lib.h>

#include "mfc-util.h"

/**
 * mfc_print_remote:
 * @channels: the enabled channels of the remote
 *
 * Print the configuration of a remote, together with the publisher and the
 * expiry date from the cached metadata of its repository.
 */
static void
mfc_print_remote(MfRemote *remote, const char *const *channels)
{
    const char *summary = mf_remote_get_summary(remote);
    const char *publisher = mf_remote_get_publisher_name(remote);
    const char *email = mf_remote_get_publisher_email(remote);
    GDateTime *valid_until = mf_remote_get_valid_until(remote);
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();
    g_autofree char *channel_list = g_strjoinv(", ", (char **) channels);

    if (summary != NULL)
        g_print("%s: %s\n", mf_remote_get_id(remote), summary);
    else
        g_print("%s\n", mf_remote_get_id(remote));
    if (publisher != NULL && email != NULL)
        /* TRANSLATORS: name and email address of the publisher of a repository */
        g_print(_("  Publisher: %s <%s>\n"), publisher, email);
    else if (publisher != NULL)
        /* TRANSLATORS: name of the publisher of a repository */
        g_print(_("  Publisher: %s\n"), publisher);
    g_print(_("  URL: %s\n"), mf_remote_get_url(remote));
    g_print(_("  Channels: %s\n"), channel_list);

    if (valid_until == NULL) {
        g_print("  %s\n", _("Not refreshed yet, run 'manifold refresh'."));
    } else {
        g_autoptr(GDateTime) local = g_date_time_to_local(valid_until);
        g_autofree char *date = g_date_time_format(local, "%Y-%m-%d %H:%M");

        if (g_date_time_compare(now, valid_until) >= 0)
            /* TRANSLATORS: %s is the date on which the metadata of a remote expired */
            g_print(_("  Expired on %s, run 'manifold refresh'.\n"), date);
        else
            /* TRANSLATORS: %s is the date on which the metadata of a remote expires */
            g_print(_("  Valid until: %s\n"), date);
    }
}

/**
 * mfc_run_remote_list_command:
 *
 * List the remotes of this system.
 */
static int
mfc_run_remote_list_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GPtrArray) remotes = NULL;
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();
    g_autoptr(GError) error = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, NULL);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc > 1) {
        mfc_print_help_hint(cmd->name, argv[1]);
        return MFC_EXIT_INVALID_CMD;
    }

    remotes = mf_remote_list_all(&error);
    if (remotes == NULL) {
        g_printerr(_("Unable to read the remotes: %s\n"), error->message);
        return MFC_EXIT_FAILURE;
    }
    if (remotes->len == 0) {
        g_print("%s\n", _("There are no remotes."));
        return MFC_EXIT_SUCCESS;
    }

    for (guint i = 0; i < remotes->len; i++) {
        if (i > 0)
            g_print("\n");
        mfc_print_remote(g_ptr_array_index(remotes, i),
                         mf_remote_get_channels(g_ptr_array_index(remotes, i)));
    }

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_register_remote_commands:
 */
void
mfc_register_remote_commands(MfcContext *ctx)
{
    mfc_context_register_command(ctx,
                                 "remote list",
                                 NULL,
                                 /* TRANSLATORS: `manifold remote list` command description */
                                 _("List the remotes that components are installed from."),
                                 mfc_run_remote_list_command);
}
