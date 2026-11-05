/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-manage.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib/gi18n-lib.h>

#include "mfc-util.h"

/**
 * mfc_on_progress:
 *
 * Draw the progress of an operation, one bar per item.
 */
static void
mfc_on_progress(const MfProgress *progress, gpointer user_data)
{
    MfcContext *ctx = user_data;
    const MfItemProgress *item = mf_progress_get_item(progress);
    g_autofree char *label = NULL;

    if (item == NULL)
        return;
    const char *subject = mf_item_progress_get_subject(item);
    const char *version = mf_item_progress_get_version(item);

    switch (mf_item_progress_get_action(item)) {
    case MF_ACTION_INSTALL:
        /* TRANSLATORS: label of a progress bar, with the component and its version */
        label = g_strdup_printf(_("Installing %s %s"), subject, version ? version : "");
        break;
    case MF_ACTION_REMOVE:
        /* TRANSLATORS: label of a progress bar, with the component and its version */
        label = g_strdup_printf(_("Removing %s %s"), subject, version ? version : "");
        break;
    case MF_ACTION_CLEANUP:
        /* TRANSLATORS: label of a progress bar, with the component and its version */
        label = g_strdup_printf(_("Cleaning up %s %s"), subject, version ? version : "");
        break;
    case MF_ACTION_REFRESH:
        /* TRANSLATORS: label of a progress bar, with the ID of a remote */
        label = g_strdup_printf(_("Refreshing %s"), subject);
        break;
    default:
        label = g_strdup(subject);
        break;
    }

    if (!mfc_progress_bar_is_for(ctx->progress_bar, item))
        mfc_progress_bar_start(ctx->progress_bar, item, label);
    mfc_progress_bar_update(ctx->progress_bar,
                            mf_item_progress_get_percent(item),
                            mf_item_progress_get_bytes_done(item));
}

/**
 * mfc_print_result:
 *
 * Print the results of an operation.
 */
static void
mfc_print_result(MfResult *result)
{
    GPtrArray *installed = mf_result_get_installed(result);
    GPtrArray *removed = mf_result_get_removed(result);
    GPtrArray *warnings = mf_result_get_warnings(result);
    guint n_images = mf_result_get_removed_images(result)->len;

    if (installed->len > 0) {
        g_print("%s\n", _("Installed:"));
        mfc_print_components(installed, FALSE);
    }
    if (removed->len > 0) {
        g_print("%s\n", _("Removed:"));
        mfc_print_components(removed, FALSE);
    }
    if (n_images > 0)
        g_print(g_dngettext(GETTEXT_PACKAGE,
                            "Removed %u image nothing uses anymore.\n",
                            "Removed %u images nothing uses anymore.\n",
                            n_images),
                n_images);
    if (mf_result_get_bytes_added(result) > 0) {
        g_autofree char *size = g_format_size(mf_result_get_bytes_added(result));

        g_print(_("Disk space used: %s\n"), size);
    }
    if (mf_result_get_bytes_freed(result) > 0) {
        g_autofree char *size = g_format_size(mf_result_get_bytes_freed(result));

        g_print(_("Disk space freed: %s\n"), size);
    }
    for (guint i = 0; i < warnings->len; i++) {
        /* a warning can quote a tool of the host, which can quote a file of a bundle */
        g_autofree char *shown = g_strescape(g_ptr_array_index(warnings, i), NULL);

        /* TRANSLATORS: something went wrong, but the operation still succeeded */
        g_printerr(_("Warning: %s\n"), shown);
    }
}

/**
 * mfc_integration_kind_get_label:
 *
 * Returns: the name of a kind of integration file, for display to the user
 */
static const char *
mfc_integration_kind_get_label(MfIntegrationKind kind)
{
    switch (kind) {
    case MF_INTEGRATION_KIND_DESKTOP_ENTRY:
        /* TRANSLATORS: kind of a file that an application installs on the system, shown in a list */
        return _("menu entry");
    case MF_INTEGRATION_KIND_MIME_PACKAGE:
        /* TRANSLATORS: kind of a file that an application installs on the system, shown in a list */
        return _("file types");
    case MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM:
        /* TRANSLATORS: kind of a file that an application installs on the system, shown in a list */
        return _("system service");
    case MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER:
        /* TRANSLATORS: kind of a file that an application installs on the system, shown in a list */
        return _("user service");
    case MF_INTEGRATION_KIND_UDEV_RULE:
        /* TRANSLATORS: kind of a file that an application installs on the system, shown in a list */
        return _("udev rule");
    default:
        return mf_integration_kind_to_string(kind);
    }
}

/**
 * mfc_print_image_integration:
 * @manifest: a manifest that passed mf_manifest_check_integration()
 * @integration: the integration section of @manifest
 *
 * List the integration files and commands that a component will install on the host.
 */
static void
mfc_print_image_integration(MfManifest *manifest, MfIntegration *integration)
{
    GPtrArray *files = mf_integration_get_files(integration);
    GPtrArray *aliases = mf_integration_get_aliases(integration);
    guint n_icons = 0;

    if (files->len == 0 && aliases->len == 0)
        return;

    /* TRANSLATORS: %s is a component, a list of the files and commands that it installs follows */
    g_print(_("%s will install this on the system:\n"), mf_manifest_get_component_id(manifest));
    for (guint i = 0; i < files->len; i++) {
        MfIntegrationFile *file = g_ptr_array_index(files, i);
        MfIntegrationKind kind = mf_integration_file_get_kind(file);
        g_autofree char *name = g_path_get_basename(mf_integration_file_get_path(file));

        /* an application ships the same icon in many sizes, so we only count them */
        if (kind == MF_INTEGRATION_KIND_ICON) {
            n_icons++;
            continue;
        }

        if (mf_integration_kind_is_privileged(kind))
            /* TRANSLATORS: the kind of a file and its name, see "Privileged items" */
            g_print(_("  %s: %s (privileged)\n"), mfc_integration_kind_get_label(kind), name);
        else
            g_print("  %s: %s\n", mfc_integration_kind_get_label(kind), name);
    }
    if (n_icons > 0)
        /* TRANSLATORS: label for the number of icons that an application installs, shown in a list */
        g_print("  %s: %u\n", _("icons"), n_icons);
    for (guint i = 0; i < aliases->len; i++) {
        MfIntegrationAlias *alias = g_ptr_array_index(aliases, i);

        /* TRANSLATORS: label for a command that an application installs, shown in a list */
        g_print("  %s: %s\n", _("command"), mf_integration_alias_get_name(alias));
    }

    if (mf_integration_is_privileged(integration))
        g_print("%s\n", _("Privileged items can gain full access to this system."));
}

/**
 * mfc_print_integration:
 * @fd: the file descriptor of a bundle
 *
 * List what a bundle will install on this system. Root is not asked before it
 * installs a bundle, and everyone else only gets the password prompts of polkit.
 *
 * This is a notice and no check, as the service decides what can be installed.
 * If a bundle is not fine, we print nothing, and the service reports the problem.
 */
static void
mfc_print_integration(int fd)
{
    g_autoptr(MfBundle) bundle = mf_bundle_new_from_fd(fd, NULL);
    GPtrArray *images;

    /* the service reports the error if the bundle can not be read */
    if (bundle == NULL)
        return;

    images = mf_bundle_get_images(bundle);
    for (guint i = 0; i < images->len; i++) {
        MfManifest *manifest = mf_bundle_image_get_manifest(g_ptr_array_index(images, i));
        MfIntegration *integration = mf_manifest_get_integration(manifest);

        /* a bundle could put terminal escapes into these names, which the check rules out */
        if (integration != NULL && mf_manifest_check_integration(manifest, NULL))
            mfc_print_image_integration(manifest, integration);
    }
}

/**
 * mfc_install_bundle:
 *
 * Install the components of one bundle file.
 */
static int
mfc_install_bundle(MfcContext *ctx, const char *fname)
{
    g_autoptr(MfResult) result = NULL;
    g_autofree char *basename = NULL;
    g_autoptr(GError) error = NULL;

    int fd = open(fname, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        g_printerr(_("Unable to open %s: %s\n"), fname, g_strerror(errno));
        return MFC_EXIT_BAD_INPUT;
    }

    mfc_print_integration(fd);

    basename = g_path_get_basename(fname);
    result = mf_manager_install_file(mfc_context_get_manager(ctx), fd, mfc_on_progress, ctx, NULL, &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    close(fd);
    if (result == NULL) {
        g_printerr(_("Unable to install %s: %s\n"), basename, error->message);
        return MFC_EXIT_FAILURE;
    }

    mfc_print_result(result);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_install_command:
 *
 * Install the components of bundle files.
 */
static int
mfc_run_install_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, NULL);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc < 2) {
        g_printerr("%s\n", _("You need to specify a bundle file to install."));
        return MFC_EXIT_BAD_INPUT;
    }

    for (int i = 1; i < argc; i++) {
        g_autoptr(GString) skipped = NULL;

        ret = mfc_install_bundle(ctx, argv[i]);
        if (ret == MFC_EXIT_SUCCESS)
            continue;

        /* we stop at the first file that fails, and list the files that were not installed */
        if (i + 1 < argc) {
            skipped = g_string_new(argv[i + 1]);
            for (int j = i + 2; j < argc; j++)
                g_string_append_printf(skipped, ", %s", argv[j]);
            g_printerr(_("The remaining files were not installed: %s\n"), skipped->str);
        }
        return ret;
    }

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_choose_version:
 *
 * If several versions of a component are installed, let the user choose
 * which one to remove.
 *
 * On success, @ret_version is set to the full version that the user chose,
 * or to %NULL if all versions should be removed.
 *
 * Returns: the exit code
 */
static int
mfc_choose_version(const char *component_id, char **ret_version)
{
    g_autoptr(MfStore) store = mf_store_new();
    g_autoptr(GPtrArray) versions = NULL;
    g_autoptr(GError) error = NULL;
    guint choice;

    *ret_version = NULL;

    versions = mf_store_list_components(store, component_id, MF_COMPONENT_KIND_UNKNOWN, &error);
    if (versions == NULL) {
        g_printerr(_("Unable to look up %s: %s\n"), component_id, error->message);
        return MFC_EXIT_FAILURE;
    }
    if (versions->len <= 1)
        return MFC_EXIT_SUCCESS;

    if (!isatty(STDIN_FILENO)) {
        g_autofree char *table = mfc_format_components(versions, FALSE);

        /* TRANSLATORS: the component, and a table of its versions */
        g_printerr(_("Several versions of %s are installed, specify the one to remove:\n%s"),
                   component_id,
                   table);
        return MFC_EXIT_FAILURE;
    }

    g_print(_("Several versions of %s are installed:\n"), component_id);
    for (guint i = 0; i < versions->len; i++) {
        MfComponent *component = g_ptr_array_index(versions, i);

        if (mf_component_is_current(component))
            /* TRANSLATORS: entry of a numbered list of versions, %u is the number to enter
             * to remove the version %s */
            g_print(_("  [%u] %s (current)\n"), i + 1, mf_component_get_full_version(component));
        else
            g_print("  [%u] %s\n", i + 1, mf_component_get_full_version(component));
    }
    /* TRANSLATORS: last entry of a numbered list of versions, %u is the number to enter
     * to remove all versions */
    g_print(_("  [%u] all of them\n"), versions->len + 1);

    choice = mfc_prompt_number(_("Which one should be removed? [0 to abort]"), versions->len + 1);
    if (choice == 0) {
        g_print("%s\n", _("Aborted."));
        return MFC_EXIT_FAILURE;
    }
    if (choice <= versions->len) {
        MfComponent *component = g_ptr_array_index(versions, choice - 1);

        *ret_version = g_strdup(mf_component_get_full_version(component));
    }

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_remove_command:
 *
 * Remove a component, or one version of it.
 */
static int
mfc_run_remove_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *chosen_version = NULL;
    const char *component_id;
    const char *version = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, NULL);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc < 2) {
        g_printerr("%s\n", _("You need to specify the component to remove."));
        return MFC_EXIT_INVALID_CMD;
    }
    if (argc > 3) {
        mfc_print_help_hint(cmd->name, argv[3]);
        return MFC_EXIT_INVALID_CMD;
    }
    component_id = argv[1];
    if (!mf_component_id_is_valid(component_id)) {
        g_printerr(_("'%s' is not a valid component identifier.\n"), component_id);
        return MFC_EXIT_INVALID_CMD;
    }
    if (argc > 2) {
        version = argv[2];
    } else {
        ret = mfc_choose_version(component_id, &chosen_version);
        if (ret != MFC_EXIT_SUCCESS)
            return ret;
        version = chosen_version;
    }

    result = mf_manager_remove_component(mfc_context_get_manager(ctx),
                                         component_id,
                                         version,
                                         mfc_on_progress,
                                         ctx,
                                         NULL,
                                         &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    if (result == NULL) {
        g_printerr(_("Unable to remove %s: %s\n"), component_id, error->message);
        return MFC_EXIT_FAILURE;
    }

    mfc_print_result(result);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_cleanup_command:
 *
 * Run a cleanup. It removes dependencies that no component needs anymore,
 * older versions of applications that are not kept, and images that no
 * component uses.
 */
static int
mfc_run_cleanup_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfResult) result = NULL;
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

    result = mf_manager_cleanup(mfc_context_get_manager(ctx), mfc_on_progress, ctx, NULL, &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    if (result == NULL) {
        g_printerr(_("Unable to clean up: %s\n"), error->message);
        return MFC_EXIT_FAILURE;
    }

    if (mf_result_get_removed(result)->len == 0 && mf_result_get_removed_images(result)->len == 0) {
        g_print("%s\n", _("Nothing to clean up."));
        return MFC_EXIT_SUCCESS;
    }
    mfc_print_result(result);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_refresh_command:
 *
 * Download the metadata of the remotes, or of some of them.
 */
static int
mfc_run_refresh_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GPtrArray) remote_ids = g_ptr_array_new();
    g_autoptr(GError) error = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, NULL);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;
    for (int i = 1; i < argc; i++)
        g_ptr_array_add(remote_ids, argv[i]);
    g_ptr_array_add(remote_ids, NULL);

    result = mf_manager_refresh(mfc_context_get_manager(ctx),
                                argc > 1 ? (const char *const *) remote_ids->pdata : NULL,
                                mfc_on_progress,
                                ctx,
                                NULL,
                                &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    if (result == NULL) {
        g_printerr(_("Unable to refresh: %s\n"), error->message);
        return MFC_EXIT_FAILURE;
    }
    mfc_print_result(result);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_mark_command:
 *
 * Set the install mode of a version of a component. A version with the mode
 * "manual" is kept until the user removes it, and a version with the mode
 * "auto" is removed by a cleanup once no component needs it anymore.
 */
static int
mfc_run_mark_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GPtrArray) marked = NULL;
    g_autoptr(GError) error = NULL;
    const char *component_id;
    MfInstallMode mode;
    int ret;

    context = mfc_option_context_for_command(cmd, NULL);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc < 3) {
        /* TRANSLATORS: 'manual' and 'auto' are arguments of the command, and not translated */
        g_printerr("%s\n", _("You need to specify 'manual' or 'auto', and the component to mark."));
        return MFC_EXIT_INVALID_CMD;
    }
    if (argc > 4) {
        mfc_print_help_hint(cmd->name, argv[4]);
        return MFC_EXIT_INVALID_CMD;
    }
    mode = mf_install_mode_from_string(argv[1]);
    if (mode == MF_INSTALL_MODE_UNKNOWN) {
        /* TRANSLATORS: 'manual' and 'auto' are arguments of the command, and not translated */
        g_printerr(_("'%s' is not a mode, it has to be 'manual' or 'auto'.\n"), argv[1]);
        return MFC_EXIT_INVALID_CMD;
    }
    component_id = argv[2];
    if (!mf_component_id_is_valid(component_id)) {
        g_printerr(_("'%s' is not a valid component identifier.\n"), component_id);
        return MFC_EXIT_INVALID_CMD;
    }

    marked = mf_manager_mark_component(mfc_context_get_manager(ctx),
                                       component_id,
                                       argc > 3 ? argv[3] : NULL,
                                       mode,
                                       NULL,
                                       &error);
    if (marked == NULL) {
        g_printerr(_("Unable to mark %s: %s\n"), component_id, error->message);
        return MFC_EXIT_FAILURE;
    }

    /* TRANSLATORS: %s is the install mode, "manual" or "auto" */
    g_print(_("Marked as %s:\n"), mf_install_mode_to_string(mode));
    mfc_print_components(marked, FALSE);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_register_manage_commands:
 */
void
mfc_register_manage_commands(MfcContext *ctx)
{
    mfc_context_register_command(ctx,
                                 "install",
                                 "FILES",
                                 /* TRANSLATORS: `manifold install` command description */
                                 _("Install the components of bundle files."),
                                 mfc_run_install_command);
    mfc_context_register_command(ctx,
                                 "remove",
                                 "COMPONENT-ID [VERSION]",
                                 /* TRANSLATORS: `manifold remove` command description */
                                 _("Remove a component, or one version of it."),
                                 mfc_run_remove_command);
    mfc_context_register_command(ctx,
                                 "cleanup",
                                 NULL,
                                 /* TRANSLATORS: `manifold cleanup` command description */
                                 _("Remove dependencies and images that are no longer needed."),
                                 mfc_run_cleanup_command);
    mfc_context_register_command(
        ctx,
        "refresh",
        "[REMOTE-ID...]",
        /* TRANSLATORS: `manifold refresh` command description */
        _("Download the metadata of the remotes, so their components can be installed."),
        mfc_run_refresh_command);
    mfc_context_register_command(
        ctx,
        "mark",
        "MODE COMPONENT-ID [VERSION]",
        /* TRANSLATORS: `manifold mark` command description, keep 'manual' and 'auto' as they are */
        _("Mark a version to keep (manual), or to remove once nothing needs it (auto)."),
        mfc_run_mark_command);
}
