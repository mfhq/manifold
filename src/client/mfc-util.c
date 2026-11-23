/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-util.h"

#include <stdio.h>
#include <string.h>
#include <glib/gi18n-lib.h>

#include "mf-utils-private.h"

#define BIN_NAME "manifold"

void
mfc_print_help_hint(const char *command, const char *unexpected_argument)
{
    if (unexpected_argument != NULL)
        g_printerr(_("Argument '%s' is not expected.\n"), unexpected_argument);

    if (command == NULL)
        /* TRANSLATORS: %s is the name of the program, 'manifold' */
        g_printerr(_("Run '%s --help' to see a full list of available command line options.\n"), BIN_NAME);
    else
        g_printerr(
            /* TRANSLATORS: %s is the name of the program, 'manifold' */
            _("Run '%s --help' to see a list of available commands and options, "
              "and '%s %s --help' to see a list of options specific for this command.\n"),
            BIN_NAME,
            BIN_NAME,
            command);
}

/**
 * mfc_option_context_for_command:
 *
 * Create the option context for a command.
 */
GOptionContext *
mfc_option_context_for_command(MfcCommand *cmd, const GOptionEntry *entries)
{
    GOptionContext *context;
    g_autofree char *parameter_str = NULL;
    g_autofree char *title = NULL;
    g_autofree char *summary = NULL;

    parameter_str = mfc_command_build_usage(cmd);
    context = g_option_context_new(parameter_str);
    g_option_context_set_help_enabled(context, TRUE);
    if (entries != NULL)
        g_option_context_add_main_entries(context, entries, NULL);

    /* TRANSLATORS: header of the --help output of a command, %s is the name of the command */
    title = g_strdup_printf(_("'%s' command"), cmd->name);
    summary = g_strdup_printf("%s\n %s\n %s", _("Manifold command-line interface"), title, cmd->summary);
    g_option_context_set_summary(context, summary);

    return context;
}

/**
 * mfc_parse_command_options:
 *
 * Parse the options of a command, printing errors.
 *
 * Returns: %MFC_EXIT_SUCCESS if the options were parsed, an error code otherwise
 */
int
mfc_parse_command_options(GOptionContext *context, MfcCommand *cmd, int *argc, char ***argv)
{
    g_autoptr(GError) error = NULL;

    if (!g_option_context_parse(context, argc, argv, &error)) {
        g_printerr("%s\n", error->message);
        mfc_print_help_hint(cmd == NULL ? NULL : cmd->name, NULL);
        return MFC_EXIT_INVALID_CMD;
    }

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_string_append_column:
 *
 * Append a column to a table row. The @text is padded with spaces to @width
 * characters, and followed by a space as column separator.
 */
void
mfc_string_append_column(GString *row, const char *text, size_t width)
{
    g_string_append(row, text);
    for (size_t len = g_utf8_strlen(text, -1); len < width; len++)
        g_string_append_c(row, ' ');
    g_string_append_c(row, ' ');
}

/**
 * mfc_format_components:
 *
 * Format a table of components, one per line.
 *
 * Returns: (transfer full): the table
 */
char *
mfc_format_components(GPtrArray *components, gboolean with_header)
{
    g_autoptr(GString) table = g_string_new(NULL);
    /* TRANSLATORS: column headers of the table of components */
    const char *kind_header = _("KIND");
    const char *id_header = _("COMPONENT");
    const char *version_header = _("VERSION");
    const char *flags_header = _("FLAGS");
    /* the kind column fits the longest kind, "platform" */
    size_t kind_width = 8;
    size_t id_width = 0;
    size_t version_width = 0;

    /* make every column as wide as its longest entry */
    if (with_header) {
        kind_width = MAX(kind_width, (size_t) g_utf8_strlen(kind_header, -1));
        id_width = g_utf8_strlen(id_header, -1);
        version_width = g_utf8_strlen(version_header, -1);
    }
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        id_width = MAX(id_width, strlen(mf_component_get_id(component)));
        version_width = MAX(version_width, strlen(mf_component_get_full_version(component)));
    }

    if (with_header) {
        mfc_string_append_column(table, kind_header, kind_width);
        mfc_string_append_column(table, id_header, id_width);
        mfc_string_append_column(table, version_header, version_width);
        g_string_append_printf(table, "%s\n", flags_header);
    }

    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);
        g_autoptr(GString) flags = g_string_new(NULL);

        if (mf_component_is_sdk(component))
            g_string_append(flags, "sdk ");
        if (mf_component_is_current(component))
            g_string_append(flags, "current ");
        if (mf_component_get_install_mode(component) == MF_INSTALL_MODE_MANUAL)
            g_string_append(flags, "manual ");
        if (flags->len > 0)
            g_string_truncate(flags, flags->len - 1);

        g_string_append_printf(table,
                               "%-*s %-*s %-*s %s\n",
                               (int) kind_width,
                               mf_component_kind_to_string(mf_component_get_kind(component)),
                               (int) id_width,
                               mf_component_get_id(component),
                               (int) version_width,
                               mf_component_get_full_version(component),
                               flags->str);
    }

    return g_string_free(g_steal_pointer(&table), FALSE);
}

/**
 * mfc_print_components:
 *
 * Print a table of components, one per line.
 */
void
mfc_print_components(GPtrArray *components, gboolean with_header)
{
    g_autofree char *table = mfc_format_components(components, with_header);

    g_print("%s", table);
}

/**
 * mfc_prompt_number:
 *
 * Ask the user for a number from 0 to @maxnum.
 *
 * Returns: the number, or 0 if the input ended
 */
guint
mfc_prompt_number(const char *question, guint maxnum)
{
    char buffer[64];
    guint answer;

    g_print("%s ", question);
    for (;;) {
        if (fgets(buffer, sizeof(buffer), stdin) == NULL)
            return 0;
        if (sscanf(buffer, "%u", &answer) == 1 && answer <= maxnum)
            return answer;
        g_print(_("Please enter a number from 0 to %u: "), maxnum);
    }
}

/**
 * mfc_prompt_yes_no:
 * @question: the question to ask, without the possible answers
 *
 * Ask a yes/no question on the terminal. The default answer is no. Only
 * "y" and "yes" count as yes, in upper or lower case.
 *
 * Returns: %TRUE if the answer was yes
 */
gboolean
mfc_prompt_yes_no(const char *question)
{
    char buffer[64];

    g_print("%s [y/N] ", question);
    if (fgets(buffer, sizeof(buffer), stdin) == NULL)
        return FALSE;
    g_strstrip(buffer);

    return g_ascii_strcasecmp(buffer, "y") == 0 || g_ascii_strcasecmp(buffer, "yes") == 0;
}

/**
 * mfc_on_progress:
 *
 * Draw the progress of an operation, one bar per item.
 */
void
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
    case MF_ACTION_UPDATE:
        /* TRANSLATORS: label of a progress bar, with the component and the version it is updated to */
        label = g_strdup_printf(_("Updating %s %s"), subject, version ? version : "");
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
 * mfc_print_updates:
 * @updates: (element-type MfUpdate): the components that an operation updated
 *
 * Print a table of updated components, one per line, with the previous and
 * the new version of each component. Below the table, we list the system
 * services and udev rules that an update added to a component or removed
 * from it.
 */
static void
mfc_print_updates(GPtrArray *updates)
{
    g_autoptr(GString) table = g_string_new(NULL);
    /* the kind column fits the longest kind, "platform" */
    size_t kind_width = 8;
    size_t id_width = 0;

    for (guint i = 0; i < updates->len; i++) {
        MfComponent *component = mf_update_get_component(g_ptr_array_index(updates, i));

        id_width = MAX(id_width, strlen(mf_component_get_id(component)));
    }

    for (guint i = 0; i < updates->len; i++) {
        MfUpdate *update = g_ptr_array_index(updates, i);
        MfComponent *component = mf_update_get_component(update);

        mfc_string_append_column(table,
                                 mf_component_kind_to_string(mf_component_get_kind(component)),
                                 kind_width);
        mfc_string_append_column(table, mf_component_get_id(component), id_width);
        g_string_append_printf(table,
                               "%s → %s\n",
                               mf_update_get_previous_version(update),
                               mf_component_get_full_version(component));
    }
    g_print("%s", table->str);

    for (guint i = 0; i < updates->len; i++) {
        MfUpdate *update = g_ptr_array_index(updates, i);
        const char *component_id = mf_component_get_id(mf_update_get_component(update));
        GPtrArray *added = mf_update_get_integration_added(update);
        GPtrArray *removed = mf_update_get_integration_removed(update);

        /* the names come from the manifest of a component, so we escape control characters */
        if (added->len > 0) {
            g_autofree char *names = mf_ptr_array_join_strings(added, ", ");
            g_autofree char *shown = g_strescape(names, NULL);

            /* TRANSLATORS: a component, and the system services and udev rules that its update added */
            g_print(_("%s now also installs: %s\n"), component_id, shown);
        }
        if (removed->len > 0) {
            g_autofree char *names = mf_ptr_array_join_strings(removed, ", ");
            g_autofree char *shown = g_strescape(names, NULL);

            /* TRANSLATORS: a component, and the system services and udev rules that its update removed */
            g_print(_("%s no longer installs: %s\n"), component_id, shown);
        }
    }
}

/**
 * mfc_print_disk_space:
 *
 * Print how much disk space of the store an operation used or freed.
 */
static void
mfc_print_disk_space(MfResult *result)
{
    uint64_t added = mf_result_get_bytes_added(result);
    uint64_t freed = mf_result_get_bytes_freed(result);
    g_autofree char *size = NULL;

    if (added == 0 && freed == 0)
        return;

    if (freed == 0) {
        size = g_format_size(added);
        g_print(_("Disk space used: %s\n"), size);
    } else if (added == 0) {
        size = g_format_size(freed);
        g_print(_("Disk space freed: %s\n"), size);
    } else if (added > freed) {
        /* the operation both added and removed images, so we show the difference */
        size = g_format_size(added - freed);
        /* TRANSLATORS: %s is the size by which the used disk space grew, like "12.0 MB" */
        g_print(_("Disk space used: %s more\n"), size);
    } else if (added < freed) {
        size = g_format_size(freed - added);
        /* TRANSLATORS: %s is the size by which the used disk space shrank, like "12.0 MB" */
        g_print(_("Disk space used: %s less\n"), size);
    } else {
        g_print("%s\n", _("Disk space used: unchanged"));
    }
}

/**
 * mfc_print_result:
 *
 * Print the results of an operation.
 */
void
mfc_print_result(MfResult *result)
{
    GPtrArray *updated = mf_result_get_updated(result);
    GPtrArray *installed = mf_result_get_installed(result);
    GPtrArray *removed = mf_result_get_removed(result);
    GPtrArray *warnings = mf_result_get_warnings(result);
    guint n_images = mf_result_get_removed_images(result)->len;

    if (updated->len > 0) {
        g_print("%s\n", _("Updated:"));
        mfc_print_updates(updated);
    }
    if (installed->len > 0) {
        g_print("%s\n", _("Installed:"));
        mfc_print_components(installed, FALSE);
    }
    if (removed->len > 0) {
        g_print("%s\n", _("Removed:"));
        mfc_print_components(removed, FALSE);
    }
    /* we do not mention removed images that only belonged to versions which an update replaced */
    if (n_images > 0 && (updated->len == 0 || removed->len > 0))
        g_print(g_dngettext(GETTEXT_PACKAGE,
                            "Removed %u image that is no longer used.\n",
                            "Removed %u images that are no longer used.\n",
                            n_images),
                n_images);
    mfc_print_disk_space(result);
    for (guint i = 0; i < warnings->len; i++) {
        /* warnings can contain text from files of a bundle, so we escape control characters */
        g_autofree char *shown = g_strescape(g_ptr_array_index(warnings, i), NULL);

        /* TRANSLATORS: %s is a problem that did not make the operation fail */
        g_printerr(_("Warning: %s\n"), shown);
    }
}
