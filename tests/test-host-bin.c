/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <sys/stat.h>
#include <glib/gstdio.h>

#include "mf-sdautoptr-private.h"
#include "mfr-host-bin.h"
#include "mft-utils.h"

/**
 * mft_write_program:
 *
 * Write an executable file.
 */
static void
mft_write_program(const char *dir, const char *name)
{
    g_autofree char *path = g_build_filename(dir, name, NULL);

    mft_write_file(path, "#!/bin/sh\n");
    g_assert_cmpint(g_chmod(path, 0755), ==, 0);
}

/**
 * mft_has_link:
 *
 * Returns: whether the links directory has a link for a program
 */
static gboolean
mft_has_link(const char *links_dir, const char *name)
{
    g_autofree char *path = g_build_filename(links_dir, name, NULL);
    g_autofree char *target = g_file_read_link(path, NULL);

    if (target == NULL)
        return FALSE;
    g_assert_cmpstr(target, ==, MFR_HOST_RUN_PATH);
    return TRUE;
}

static void
test_sync(void)
{
    g_autofree char *tmp = mft_make_tmp_dir("host-bin");
    g_autofree char *links_dir = g_build_filename(tmp, "links", NULL);
    g_autofree char *bin_dir = g_build_filename(tmp, "bin", NULL);
    g_autofree char *missing_dir = g_build_filename(tmp, "missing", NULL);
    g_autofree char *data_path = g_build_filename(bin_dir, "data", NULL);
    g_autofree char *alias_path = g_build_filename(bin_dir, "alias", NULL);
    g_autofree char *broken_path = g_build_filename(bin_dir, "broken", NULL);
    g_autofree char *subdir = g_build_filename(bin_dir, "subdir", NULL);
    g_autofree char *tool_path = g_build_filename(bin_dir, "tool", NULL);
    const char *const program_dirs[] = {bin_dir, missing_dir, NULL};
    g_autoptr(MfrHostBin) host_bin = NULL;
    g_autoptr(GError) error = NULL;

    mft_make_dirs(subdir);
    mft_write_program(bin_dir, "tool");
    mft_write_file(data_path, "no program");
    mft_make_link("tool", alias_path);
    mft_make_link("nothing", broken_path);

    host_bin = mfr_host_bin_new(NULL, links_dir, program_dirs, &error);
    g_assert_no_error(error);
    g_assert_true(mft_has_link(links_dir, "tool"));
    g_assert_true(mft_has_link(links_dir, "alias"));
    g_assert_false(mft_has_link(links_dir, "data"));
    g_assert_false(mft_has_link(links_dir, "broken"));
    g_assert_false(mft_has_link(links_dir, "subdir"));

    /* the link of a removed program is removed, and the programs of a new directory get links */
    g_assert_cmpint(g_unlink(tool_path), ==, 0);
    mft_make_dirs(missing_dir);
    mft_write_program(missing_dir, "other");
    g_assert_true(mfr_host_bin_sync(host_bin, &error));
    g_assert_no_error(error);
    g_assert_false(mft_has_link(links_dir, "tool"));
    g_assert_false(mft_has_link(links_dir, "alias"));
    g_assert_true(mft_has_link(links_dir, "other"));

    mft_rm_rf(tmp);
}

/**
 * mft_wait_for_link:
 *
 * Run the event loop until the link of a program was created, or until it
 * was removed if @expected is %FALSE.
 */
static void
mft_wait_for_link(sd_event *event, const char *links_dir, const char *name, gboolean expected)
{
    gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

    while (mft_has_link(links_dir, name) != expected) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_assert_cmpint(sd_event_run(event, 100 * 1000), >=, 0);
    }
}

static void
test_watch(void)
{
    g_autofree char *tmp = mft_make_tmp_dir("host-bin");
    g_autofree char *links_dir = g_build_filename(tmp, "links", NULL);
    g_autofree char *bin_dir = g_build_filename(tmp, "bin", NULL);
    g_autofree char *late_dir = g_build_filename(tmp, "late", NULL);
    g_autofree char *tool_path = g_build_filename(bin_dir, "tool", NULL);
    const char *const program_dirs[] = {bin_dir, late_dir, NULL};
    g_autoptr(sd_event) event = NULL;
    g_autoptr(MfrHostBin) host_bin = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_cmpint(sd_event_new(&event), >=, 0);
    mft_make_dirs(bin_dir);
    host_bin = mfr_host_bin_new(event, links_dir, program_dirs, &error);
    g_assert_no_error(error);

    mft_write_program(bin_dir, "tool");
    mft_wait_for_link(event, links_dir, "tool", TRUE);
    g_assert_cmpint(g_unlink(tool_path), ==, 0);
    mft_wait_for_link(event, links_dir, "tool", FALSE);

    /* a program directory that is created later is found, and is watched from then on */
    mft_make_dirs(late_dir);
    mft_write_program(late_dir, "first");
    mft_wait_for_link(event, links_dir, "first", TRUE);
    mft_write_program(late_dir, "second");
    mft_wait_for_link(event, links_dir, "second", TRUE);

    mft_rm_rf(tmp);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/host-bin/sync", test_sync);
    g_test_add_func("/host-bin/watch", test_watch);

    return g_test_run();
}
