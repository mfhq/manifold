/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-remote.h"

#include <unistd.h>
#include <glib/gi18n-lib.h>

#include "mfc-util.h"

/* the values of the options of the remote add command */
static char *optn_key = NULL;
static char **optn_channels = NULL;
static gboolean optn_yes = FALSE;

static const GOptionEntry remote_add_options[] = {
    {"key",
     0, 0,
     G_OPTION_ARG_FILENAME,     &optn_key,
     N_("Trust the public key in this file, instead of the key the repository serves."),
     "FILE"},
    {"channel",
     0, 0,
     G_OPTION_ARG_STRING_ARRAY, &optn_channels,
     N_("Install from this channel instead of 'main'. Can be given more than once."),
     "NAME"},
    {"yes",
     0, 0,
     G_OPTION_ARG_NONE,         &optn_yes,
     N_("Trust the key the repository serves without asking."),
     NULL  },
    G_OPTION_ENTRY_NULL,
};

/**
 * mfc_print_remote:
 * @channels: the channels the remote installs from
 *
 * Print a remote: its definition, and the description of its repository
 * from the last refresh.
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
        /* TRANSLATORS: who publishes a repository, with an email address */
        g_print(_("  Publisher: %s <%s>\n"), publisher, email);
    else if (publisher != NULL)
        /* TRANSLATORS: who publishes a repository */
        g_print(_("  Publisher: %s\n"), publisher);
    g_print(_("  URL: %s\n"), mf_remote_get_url(remote));
    g_print(_("  Channels: %s\n"), channel_list);

    if (valid_until == NULL) {
        g_print("  %s\n", _("Not refreshed yet, run 'manifold refresh'."));
    } else {
        g_autoptr(GDateTime) local = g_date_time_to_local(valid_until);
        g_autofree char *date = g_date_time_format(local, "%Y-%m-%d %H:%M");

        if (g_date_time_compare(now, valid_until) >= 0)
            /* TRANSLATORS: %s is a date, after which nothing is installed from a remote */
            g_print(_("  Expired on %s, run 'manifold refresh'.\n"), date);
        else
            /* TRANSLATORS: %s is a date, after which nothing is installed from a remote */
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
 * mfc_read_public_key:
 *
 * Returns: (transfer full): the text of the public key file @path, or %NULL
 */
static char *
mfc_read_public_key(const char *path)
{
    g_autoptr(GError) error = NULL;
    char *contents = NULL;

    if (!g_file_get_contents(path, &contents, NULL, &error)) {
        g_printerr(_("Unable to read %s: %s\n"), path, error->message);
        return NULL;
    }
    return contents;
}

/**
 * mfc_check_remote_info:
 * @channels: the channels the remote would install from
 *
 * Check that the repository can be added as a remote with @channels. We do
 * this before we ask the user to trust the key, as the service would refuse
 * the remote anyway.
 *
 * Returns: %TRUE if the repository can be added
 */
static gboolean
mfc_check_remote_info(MfRemoteInfo *info, const char *const *channels)
{
    const char *remote_id = mf_remote_get_id(mf_remote_info_get_remote(info));
    const char *const *available = mf_remote_info_get_available_channels(info);
    g_autoptr(MfRemote) existing = mf_remote_new_for_id(remote_id, NULL);

    if (existing != NULL) {
        g_printerr(_("There is a remote %s already.\n"), remote_id);
        return FALSE;
    }
    for (guint i = 0; channels[i] != NULL; i++) {
        if (!g_strv_contains(available, channels[i])) {
            /* TRANSLATORS: the repository, and a channel it does not have */
            g_printerr(_("The repository %s has no channel %s for this system.\n"), remote_id, channels[i]);
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * mfc_run_remote_add_command:
 *
 * Add a repository as a remote, once the user trusts the key that signs it.
 */
static int
mfc_run_remote_add_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    const char *const default_channels[] = {"main", NULL};
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfRemoteInfo) info = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *public_key = NULL;
    g_autofree char *channel_list = NULL;
    const char *const *channels;
    const char *url;
    MfRemote *remote;
    int ret;

    context = mfc_option_context_for_command(cmd, remote_add_options);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc < 2) {
        g_printerr("%s\n", _("You need to specify the URL of the repository."));
        return MFC_EXIT_INVALID_CMD;
    }
    if (argc > 2) {
        mfc_print_help_hint(cmd->name, argv[2]);
        return MFC_EXIT_INVALID_CMD;
    }
    url = argv[1];
    channels = optn_channels != NULL ? (const char *const *) optn_channels : default_channels;

    /* the user has to decide whether to trust the key, and we can only ask on a terminal */
    if (optn_key == NULL && !optn_yes && !isatty(STDIN_FILENO)) {
        /* TRANSLATORS: --yes and --key are options of the command, and not translated */
        g_printerr("%s\n",
                   _("Without a terminal to ask on, trust the key with --yes, or give it with --key."));
        return MFC_EXIT_INVALID_CMD;
    }
    if (optn_key != NULL) {
        public_key = mfc_read_public_key(optn_key);
        if (public_key == NULL)
            return MFC_EXIT_BAD_INPUT;
    }

    info = mf_manager_inspect_remote(mfc_context_get_manager(ctx), url, public_key, NULL, &error);
    if (info == NULL) {
        g_printerr(_("Unable to add the repository at %s: %s\n"), url, error->message);
        return MFC_EXIT_FAILURE;
    }
    remote = mf_remote_info_get_remote(info);
    if (!mfc_check_remote_info(info, channels))
        return MFC_EXIT_FAILURE;
    mfc_print_remote(remote, channels);
    /* TRANSLATORS: the fingerprint of the key that signs a repository */
    g_print(_("  Key: %s\n"), mf_remote_info_get_fingerprint(info));

    /* the user trusts the key already if it was given with --key, or if --yes was passed */
    if (optn_key == NULL && !optn_yes) {
        if (!mfc_prompt_yes_no(_("Trust this key, and add the remote?"))) {
            g_print("%s\n", _("Aborted."));
            return MFC_EXIT_FAILURE;
        }
    }

    result = mf_manager_add_remote(mfc_context_get_manager(ctx),
                                   mf_remote_get_id(remote),
                                   mf_remote_get_url(remote),
                                   mf_remote_info_get_public_key(info),
                                   channels,
                                   mfc_on_progress,
                                   ctx,
                                   NULL,
                                   &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    if (result == NULL) {
        g_printerr(_("Unable to add the remote %s: %s\n"), mf_remote_get_id(remote), error->message);
        return MFC_EXIT_FAILURE;
    }

    channel_list = g_strjoinv(", ", (char **) channels);
    /* TRANSLATORS: the new remote, and the channels it installs from */
    g_print(_("Added the remote %s, which installs from %s.\n"), mf_remote_get_id(remote), channel_list);
    mfc_print_result(result);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_remote_remove_command:
 *
 * Remove a remote. Components that were installed from it stay installed.
 */
static int
mfc_run_remote_remove_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, NULL);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc < 2) {
        g_printerr("%s\n", _("You need to specify the remote to remove."));
        return MFC_EXIT_INVALID_CMD;
    }
    if (argc > 2) {
        mfc_print_help_hint(cmd->name, argv[2]);
        return MFC_EXIT_INVALID_CMD;
    }

    if (!mf_manager_remove_remote(mfc_context_get_manager(ctx), argv[1], NULL, &error)) {
        g_printerr(_("Unable to remove the remote %s: %s\n"), argv[1], error->message);
        return MFC_EXIT_FAILURE;
    }

    g_print(_("Removed the remote %s. What was installed from it stays installed, but is not updated "
              "anymore.\n"),
            argv[1]);

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
    mfc_context_register_command(ctx,
                                 "remote add",
                                 "URL",
                                 /* TRANSLATORS: `manifold remote add` command description */
                                 _("Add a repository as a remote, once you trust the key that signs it."),
                                 mfc_run_remote_add_command);
    mfc_context_register_command(ctx,
                                 "remote remove",
                                 "REMOTE-ID",
                                 /* TRANSLATORS: `manifold remote remove` command description */
                                 _("Remove a remote. What was installed from it stays installed."),
                                 mfc_run_remote_remove_command);
}
