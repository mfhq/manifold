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
#include <glib/gstdio.h>

#include "mfc-util.h"

/* the value of the --origin option of the install command */
static char *optn_origin = NULL;

static const GOptionEntry install_options[] = {
    {"origin",
     0, 0,
     G_OPTION_ARG_STRING, &optn_origin,
     N_("Install components from this origin, '<remote>-<channel>', if more than one has them."),
     "ORIGIN"},
    G_OPTION_ENTRY_NULL,
};

/* the value of the --no-refresh option of the update command */
static gboolean optn_no_refresh = FALSE;

static const GOptionEntry update_options[] = {
    {"no-refresh",
     0, 0,
     G_OPTION_ARG_NONE, &optn_no_refresh,
     N_("Do not refresh the remotes first, and update from the cached metadata."),
     NULL},
    G_OPTION_ENTRY_NULL,
};

/**
 * mfc_argument_is_bundle_file:
 *
 * Check if an argument of the install command is the path of a bundle file.
 *
 * A bundle file has to be given as a path that starts with `/`, `./` or `../`.
 * A plain name is always a component ID, even if a file with that name exists
 * in the current directory.
 *
 * Returns: %TRUE if @argument is a bundle file, %FALSE if it is a component ID
 */
static gboolean
mfc_argument_is_bundle_file(const char *argument)
{
    return g_str_has_prefix(argument, "/") || g_str_has_prefix(argument, "./") ||
           g_str_has_prefix(argument, "../");
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
}

/**
 * mfc_permission_get_label:
 *
 * Returns: a description of a permission, for display to the user
 */
static const char *
mfc_permission_get_label(MfPermission permission)
{
    switch (permission) {
    case MF_PERMISSION_HOST_COMMANDS:
        /* TRANSLATORS: a permission of an application, in a list */
        return _("run programs of this system");
    case MF_PERMISSION_HOST_TMP:
        /* TRANSLATORS: a permission of an application, in a list */
        return _("share /tmp with this system");
    case MF_PERMISSION_SYSTEM_SERVICES:
        /* TRANSLATORS: a permission of an application, in a list */
        return _("install system services");
    case MF_PERMISSION_UDEV_RULES:
        /* TRANSLATORS: a permission of an application, in a list */
        return _("install udev rules");
    default:
        return mf_permission_to_string(permission);
    }
}

/**
 * mfc_print_image_permissions:
 * @manifest: a manifest that passed mf_manifest_check_permissions()
 *
 * List the permissions of a component. The list has the permissions that the
 * manifest requests and the permissions that its integration section implies.
 */
static void
mfc_print_image_permissions(MfManifest *manifest)
{
    gboolean have_header = FALSE;

    for (MfPermission permission = MF_PERMISSION_UNKNOWN + 1; permission < MF_PERMISSION_LAST; permission++) {
        if (!mf_manifest_has_permission(manifest, permission))
            continue;
        if (!have_header) {
            /* TRANSLATORS: %s is a component, and a list of its permissions follows */
            g_print(_("%s requests these permissions:\n"), mf_manifest_get_component_id(manifest));
            have_header = TRUE;
        }
        if (mf_permission_is_privileged(permission))
            /* TRANSLATORS: a permission of an application, see "Privileged permissions" */
            g_print(_("  %s (privileged)\n"), mfc_permission_get_label(permission));
        else
            g_print("  %s\n", mfc_permission_get_label(permission));
    }

    if (mf_manifest_is_privileged(manifest))
        g_print("%s\n", _("Privileged permissions can give full access to this system."));
}

/**
 * mfc_print_bundle_notice:
 * @fd: the file descriptor of a bundle
 *
 * Print the integration files that the components of a bundle will install
 * on the host, and the permissions that the components request.
 *
 * The notice is only information for the user, the service decides on its own
 * whether a bundle can be installed.
 */
static void
mfc_print_bundle_notice(int fd)
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

        /* we only print names from a manifest that passed its check, as the check rejects
         * names with terminal escape sequences */
        if (integration != NULL && mf_manifest_check_integration(manifest, NULL))
            mfc_print_image_integration(manifest, integration);
        if (mf_manifest_check_permissions(manifest, NULL))
            mfc_print_image_permissions(manifest);
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

    g_autofd int fd = open(fname, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        g_printerr(_("Unable to open %s: %s\n"), fname, g_strerror(errno));
        return MFC_EXIT_BAD_INPUT;
    }

    mfc_print_bundle_notice(fd);

    basename = g_path_get_basename(fname);
    result = mf_manager_install_file(mfc_context_get_manager(ctx), fd, mfc_on_progress, ctx, NULL, &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    if (result == NULL) {
        g_printerr(_("Unable to install %s: %s\n"), basename, error->message);
        return MFC_EXIT_FAILURE;
    }

    mfc_print_result(result);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_install_component:
 * @origin: (nullable): the origin to install from, or %NULL if only one has the component
 *
 * Install a component from the remotes, with the bases it needs.
 */
static int
mfc_install_component(MfcContext *ctx, const char *component_id, const char *origin)
{
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    result = mf_manager_install_component(mfc_context_get_manager(ctx),
                                          component_id,
                                          origin,
                                          mfc_on_progress,
                                          ctx,
                                          NULL,
                                          &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    if (result == NULL) {
        g_printerr(_("Unable to install %s: %s\n"), component_id, error->message);
        if (g_error_matches(error, MF_ERROR, MF_ERROR_AMBIGUOUS))
            /* TRANSLATORS: --origin is an option of the command, and not translated */
            g_printerr("%s\n", _("Choose one of them with --origin."));
        return MFC_EXIT_FAILURE;
    }

    mfc_print_result(result);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_install_command:
 *
 * Install the components of bundle files, and components from the remotes.
 */
static int
mfc_run_install_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    guint n_components = 0;
    int ret;

    context = mfc_option_context_for_command(cmd, install_options);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc < 2) {
        g_printerr("%s\n", _("You need to specify a bundle file or a component to install."));
        return MFC_EXIT_BAD_INPUT;
    }

    /* validate all arguments before we install anything */
    for (int i = 1; i < argc; i++) {
        if (mfc_argument_is_bundle_file(argv[i]))
            continue;
        if (!mf_component_id_is_valid(argv[i])) {
            g_printerr(_("'%s' is not a valid component identifier. The path of a bundle file has to "
                         "start with ./ or /\n"),
                       argv[i]);
            return MFC_EXIT_BAD_INPUT;
        }
        n_components++;
    }
    if (optn_origin != NULL && n_components == 0) {
        g_printerr("%s\n",
                   _("An origin can only be chosen for components from the remotes, not for bundle files."));
        return MFC_EXIT_INVALID_CMD;
    }

    for (int i = 1; i < argc; i++) {
        g_autoptr(GString) skipped = NULL;

        ret = mfc_argument_is_bundle_file(argv[i]) ? mfc_install_bundle(ctx, argv[i])
                                                   : mfc_install_component(ctx, argv[i], optn_origin);
        if (ret == MFC_EXIT_SUCCESS)
            continue;

        /* we stop at the first argument that fails, and list the arguments that were not installed */
        if (i + 1 < argc) {
            skipped = g_string_new(argv[i + 1]);
            for (int j = i + 2; j < argc; j++)
                g_string_append_printf(skipped, ", %s", argv[j]);
            g_printerr(_("The rest was not installed: %s\n"), skipped->str);
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
 * mfc_refresh_before_update:
 *
 * Refresh all remotes before we update.
 *
 * A failed refresh is only a warning, the update then uses the cached metadata.
 */
static void
mfc_refresh_before_update(MfcContext *ctx)
{
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    result = mf_manager_refresh(mfc_context_get_manager(ctx), NULL, mfc_on_progress, ctx, NULL, &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    if (result == NULL) {
        /* TRANSLATORS: the update continues after this warning, %s is the reason why the refresh failed */
        g_printerr(_("Warning: Unable to refresh, updating from the cached metadata: %s\n"), error->message);
        return;
    }
    mfc_print_result(result);
}

/**
 * mfc_run_update_command:
 *
 * Update all components that were installed from remotes, or only the named ones.
 */
static int
mfc_run_update_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GPtrArray) component_ids = g_ptr_array_new();
    g_autoptr(GError) error = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, update_options);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;
    for (int i = 1; i < argc; i++) {
        if (!mf_component_id_is_valid(argv[i])) {
            g_printerr(_("'%s' is not a valid component identifier.\n"), argv[i]);
            return MFC_EXIT_INVALID_CMD;
        }
        g_ptr_array_add(component_ids, argv[i]);
    }
    g_ptr_array_add(component_ids, NULL);

    if (!optn_no_refresh)
        mfc_refresh_before_update(ctx);

    result = mf_manager_update(mfc_context_get_manager(ctx),
                               argc > 1 ? (const char *const *) component_ids->pdata : NULL,
                               mfc_on_progress,
                               ctx,
                               NULL,
                               &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    if (result == NULL) {
        g_printerr(_("Unable to update: %s\n"), error->message);
        return MFC_EXIT_FAILURE;
    }

    /* components that could not be updated are listed in the warnings, the command still succeeds */
    if (mf_result_get_updated(result)->len == 0 && mf_result_get_installed(result)->len == 0 &&
        mf_result_get_warnings(result)->len == 0) {
        g_print("%s\n", _("There are no updates."));
        return MFC_EXIT_SUCCESS;
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
                                 "FILE|COMPONENT-ID...",
                                 /* TRANSLATORS: `manifold install` command description */
                                 _("Install bundle files, or components from the remotes."),
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
    mfc_context_register_command(ctx,
                                 "update",
                                 "[COMPONENT-ID...]",
                                 /* TRANSLATORS: `manifold update` command description */
                                 _("Update the components that were installed from remotes."),
                                 mfc_run_update_command);
    mfc_context_register_command(
        ctx,
        "mark",
        "MODE COMPONENT-ID [VERSION]",
        /* TRANSLATORS: `manifold mark` command description, keep 'manual' and 'auto' as they are */
        _("Mark a version to keep (manual), or to remove once nothing needs it (auto)."),
        mfc_run_mark_command);
}
