/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <inttypes.h>
#include <sys/stat.h>
#include <glib/gstdio.h>

#include "mfr-capture.h"
#include "mft-sandbox.h"
#include "mft-utils.h"

/*
 * Tests of the cache of captured host libraries, which run in the sandbox.
 * They check which captures the compose daemon keeps when it starts, and
 * which captures it removes.
 *
 * Creating a real capture needs a mounted core, so the tests write the files
 * of a capture directly, in the layout that the daemon uses, but without any
 * captured libraries.
 */

#define TEST_CAPTURES_DIR MF_RUN_CACHE_DIR "/capture"

/**
 * mft_dir_entry:
 *
 * Make the stamp entry of a host directory, with the current modification
 * time of the directory.
 */
static char *
mft_dir_entry(const char *path)
{
    struct stat st;
    uint64_t mtime = 0;

    if (stat(path, &st) == 0)
        mtime = (uint64_t) st.st_mtim.tv_sec * G_USEC_PER_SEC * 1000 + (uint64_t) st.st_mtim.tv_nsec;
    return g_strdup_printf("{\"path\": \"%s\", \"mtime\": %" PRIu64 "}", path, mtime);
}

/**
 * mft_write_capture:
 * @dirs_json: (nullable): the host directories of the stamp, or %NULL to write no stamp
 * @targets_json: the host libraries of the stamp
 *
 * Write a capture without libraries.
 *
 * Returns: (transfer full): the directory of the capture
 */
static char *
mft_write_capture(const char *name, const char *dirs_json, const char *targets_json)
{
    char *dir = g_build_filename(TEST_CAPTURES_DIR, name, NULL);
    g_autofree char *overrides = g_build_filename(dir, "overrides", NULL);
    g_autofree char *stamp_path = g_build_filename(dir, "stamp", NULL);
    g_autofree char *environment_path = g_build_filename(dir, "environment.json", NULL);
    /* clang-format off */
    g_autofree char *stamp = g_strdup_printf(
        "{\n"
        "   \"id\": \"%s\",\n"
        "   \"tuple\": \"x86_64-linux-gnu\",\n"
        "   \"dirs\": %s,\n"
        "   \"targets\": %s\n"
        "}",
        name,
        dirs_json != NULL ? dirs_json : "[]",
        targets_json);
    /* clang-format on */

    mft_make_dirs(overrides);
    if (dirs_json != NULL)
        mft_write_file(stamp_path, stamp);
    mft_write_file(environment_path, "{\"VK_DRIVER_FILES\": \"\"}");
    return dir;
}

/**
 * mft_reset_captures:
 *
 * Remove all captures.
 */
static void
mft_reset_captures(void)
{
    mft_rm_rf(TEST_CAPTURES_DIR);
    mft_make_dirs(TEST_CAPTURES_DIR);
}

/**
 * test_check_all:
 *
 * When the daemon starts, it removes the captures that do not match the
 * host anymore, and the captures that are not complete.
 */
static void
test_check_all(void)
{
    g_autofree char *usr_lib = mft_dir_entry("/usr/lib");
    g_autofree char *fresh_dirs = g_strdup_printf("[%s]", usr_lib);
    /* a library that was missing when the capture was made has the modification time 0 */
    const char *missing_library = "[{\"path\": \"/usr/lib/does-not-exist.so\", \"size\": 1, \"mtime\": 0}]";
    const char *removed_library = "[{\"path\": \"/usr/lib/does-not-exist.so\", \"size\": 1, \"mtime\": 1}]";
    g_autofree char *fresh = NULL;
    g_autofree char *still_missing = NULL;
    g_autofree char *changed_dir = NULL;
    g_autofree char *removed = NULL;
    g_autofree char *no_stamp = NULL;
    g_autofree char *bad_stamp = NULL;
    g_autofree char *bad_stamp_path = NULL;
    g_autofree char *unfinished = g_build_filename(TEST_CAPTURES_DIR, ".tmp-unfinished", NULL);

    mft_reset_captures();

    /* captures that match the host */
    fresh = mft_write_capture("fresh", fresh_dirs, "[]");
    still_missing = mft_write_capture("still-missing", fresh_dirs, missing_library);

    /* a host directory changed, and a captured library was removed */
    changed_dir = mft_write_capture("changed-dir", "[{\"path\": \"/usr/lib\", \"mtime\": 1}]", "[]");
    removed = mft_write_capture("removed-library", fresh_dirs, removed_library);

    /* captures without a stamp, with a stamp that lacks fields, and one that was never finished */
    no_stamp = mft_write_capture("no-stamp", NULL, "[]");
    bad_stamp = mft_write_capture("bad-stamp", "[]", "[]");
    bad_stamp_path = g_build_filename(bad_stamp, "stamp", NULL);
    mft_write_file(bad_stamp_path, "{\"id\": \"x\"}");
    mft_make_dirs(unfinished);

    mfr_captures_check_all(NULL);

    g_assert_true(g_file_test(fresh, G_FILE_TEST_IS_DIR));
    g_assert_true(g_file_test(still_missing, G_FILE_TEST_IS_DIR));
    g_assert_false(g_file_test(changed_dir, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(removed, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(no_stamp, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(bad_stamp, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(unfinished, G_FILE_TEST_EXISTS));
}

/**
 * test_discard_all:
 *
 * While the daemon runs, a discarded capture is only moved aside, because a
 * tree that was composed with the capture may still be sent to a runner.
 * The captures that were moved aside are deleted when the daemon starts.
 */
static void
test_discard_all(void)
{
    g_autofree char *usr_lib = mft_dir_entry("/usr/lib");
    g_autofree char *dirs = g_strdup_printf("[%s]", usr_lib);
    g_autofree char *one = NULL;
    g_autofree char *two = NULL;
    g_autofree char *one_aside = g_build_filename(TEST_CAPTURES_DIR, ".stale-one", NULL);
    g_autofree char *two_aside = g_build_filename(TEST_CAPTURES_DIR, ".stale-two", NULL);
    g_autoptr(MfrCaptures) captures = mfr_captures_new();
    g_autoptr(GError) error = NULL;

    mft_reset_captures();

    one = mft_write_capture("one", dirs, "[]");
    two = mft_write_capture("two", dirs, "[]");

    g_assert_true(mfr_captures_discard_all(captures, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(one, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(two, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(one_aside, G_FILE_TEST_IS_DIR));
    g_assert_true(g_file_test(two_aside, G_FILE_TEST_IS_DIR));

    mfr_captures_check_all(NULL);
    g_assert_false(g_file_test(one_aside, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(two_aside, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(TEST_CAPTURES_DIR, G_FILE_TEST_IS_DIR));
}

int
main(int argc, char **argv)
{
    mft_sandbox_enter();
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/capture/check-all", test_check_all);
    g_test_add_func("/capture/discard-all", test_discard_all);

    return g_test_run();
}
