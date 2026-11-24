/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include "config.h"
#include "mfr-host-bin.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libglnx.h>

#include "mf-constants-private.h"
#include "mf-host-links.h"
#include "mf-store-private.h"
#include "mf-utils-private.h"

/**
 * MfrHostBin:
 *
 * A directory with a link for every program of the host. Each link points to
 * our trampoline program, which asks the host to run the actual program.
 * We append the directory to the end of the search path within our container,
 * so that a program that is missing in the tree is run on the host.
 *
 * There is one directory for all trees. We watch the program directories of
 * the host and update the links when they change (so we don't slow down app launches).
 */

/* the program directories of the host that we create links for by default */
static const char *const DEFAULT_PROGRAM_DIRS[] = {
    "/usr/local/sbin",
    "/usr/local/bin",
    "/usr/sbin",
    "/usr/bin",
    "/sbin",
    "/bin",
    "/usr/games",
    /* the commands of other Manifold applications */
    MF_STORAGE_DIR "/" MF_STORE_EXPORTS_NAME "/bin",
    NULL,
};

/* the time we wait after a change before we update the links. We wait
 * because installing a package changes many files in a short time. */
constexpr uint64_t SYNC_DELAY_USEC = 500 * 1000;

/* the inotify events of a program directory that can mean a program was added or removed */
constexpr uint32_t PROGRAM_DIR_EVENTS = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                                        IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

/**
 * MfrHostBinWatch:
 *
 * The watch on one program directory of the host. While the directory does
 * not exist, we watch its parent directory instead, to notice when it is created.
 */
typedef struct {
    MfrHostBin *host_bin;           /* (unowned) */
    const char *dir;                /* (unowned) */
    sd_event_source *source;        /* (nullable) */
    sd_event_source *parent_source; /* (nullable) */
} MfrHostBinWatch;

struct _MfrHostBin {
    GObject parent_instance;

    char *links_dir;
    char **program_dirs;
    sd_event *event;             /* (nullable) */
    GArray *watches;             /* of MfrHostBinWatch, one for each program directory */
    sd_event_source *sync_timer; /* (nullable) */
};

G_DEFINE_FINAL_TYPE(MfrHostBin, mfr_host_bin, G_TYPE_OBJECT)

/**
 * mfr_host_bin_watch_clear:
 */
static void
mfr_host_bin_watch_clear(MfrHostBinWatch *watch)
{
    g_clear_pointer(&watch->source, sd_event_source_disable_unref);
    g_clear_pointer(&watch->parent_source, sd_event_source_disable_unref);
}

/**
 * mfr_host_bin_init:
 */
static void
mfr_host_bin_init(MfrHostBin *host_bin)
{
    host_bin->watches = g_array_new(FALSE, TRUE, sizeof(MfrHostBinWatch));
    g_array_set_clear_func(host_bin->watches, (GDestroyNotify) mfr_host_bin_watch_clear);
}

/**
 * mfr_host_bin_finalize:
 */
static void
mfr_host_bin_finalize(GObject *object)
{
    MfrHostBin *host_bin = MFR_HOST_BIN(object);

    g_array_unref(host_bin->watches);
    g_clear_pointer(&host_bin->sync_timer, sd_event_source_disable_unref);
    g_clear_pointer(&host_bin->event, sd_event_unref);
    g_free(host_bin->links_dir);
    g_strfreev(host_bin->program_dirs);

    G_OBJECT_CLASS(mfr_host_bin_parent_class)->finalize(object);
}

/**
 * mfr_host_bin_class_init:
 */
static void
mfr_host_bin_class_init(MfrHostBinClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mfr_host_bin_finalize;
}

/**
 * mfr_host_bin_sync:
 * @host_bin: the links
 * @error: a #GError
 *
 * Update the links to match the programs that the host has now. A program
 * directory that does not exist is skipped.
 */
gboolean
mfr_host_bin_sync(MfrHostBin *host_bin, GError **error)
{
    g_autoptr(GArray) dir_fds = g_array_new(FALSE, FALSE, sizeof(int));
    g_autofd int links_fd = -1;
    int r;

    g_return_val_if_fail(MFR_IS_HOST_BIN(host_bin), FALSE);

    g_array_set_clear_func(dir_fds, (GDestroyNotify) mf_clear_fd);
    for (char **dir = host_bin->program_dirs; *dir != NULL; dir++) {
        int fd = open(*dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

        if (fd >= 0)
            g_array_append_val(dir_fds, fd);
        else if (errno != ENOENT && errno != ENOTDIR)
            return glnx_throw_errno_prefix(error, "Unable to open %s", *dir);
    }

    links_fd = open(host_bin->links_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (links_fd < 0)
        return glnx_throw_errno_prefix(error, "Unable to open %s", host_bin->links_dir);

    r = mf_host_links_sync(links_fd, (const int *) dir_fds->data, dir_fds->len);
    if (r < 0) {
        errno = -r;
        return glnx_throw_errno_prefix(error, "Unable to update the links in %s", host_bin->links_dir);
    }

    return TRUE;
}

/**
 * mfr_host_bin_schedule_sync:
 *
 * Schedule an update of the links after a short delay. Calling this function
 * again before the update has run restarts the delay.
 */
static void
mfr_host_bin_schedule_sync(MfrHostBin *host_bin)
{
    int r = sd_event_source_set_time_relative(host_bin->sync_timer, SYNC_DELAY_USEC);

    if (r >= 0)
        r = sd_event_source_set_enabled(host_bin->sync_timer, SD_EVENT_ONESHOT);
    if (r < 0)
        g_warning("Unable to schedule an update of %s: %s", host_bin->links_dir, g_strerror(-r));
}

/**
 * mfr_host_bin_on_dir_changed:
 *
 * Called when a file was added, removed or changed in a program directory of the host.
 */
static int
mfr_host_bin_on_dir_changed(sd_event_source *source, const struct inotify_event *event, void *userdata)
{
    MfrHostBinWatch *watch = userdata;

    /* the directory itself was removed, so we drop its watch and wait for it to be created again */
    if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) != 0)
        g_clear_pointer(&watch->source, sd_event_source_disable_unref);

    mfr_host_bin_schedule_sync(watch->host_bin);
    return 0;
}

/**
 * mfr_host_bin_on_parent_changed:
 *
 * Called when an entry was created in the parent directory of a program
 * directory that does not exist.
 */
static int
mfr_host_bin_on_parent_changed(sd_event_source *source, const struct inotify_event *event, void *userdata)
{
    MfrHostBinWatch *watch = userdata;
    g_autofree char *name = g_path_get_basename(watch->dir);

    if (event->len > 0 && g_str_equal(event->name, name))
        mfr_host_bin_schedule_sync(watch->host_bin);
    return 0;
}

/**
 * mfr_host_bin_watch_update:
 *
 * Watch a program directory, or its parent directory if it does not exist.
 * We only warn if the directory can not be watched, because the existing links still work.
 */
static void
mfr_host_bin_watch_update(MfrHostBinWatch *watch)
{
    sd_event *event = watch->host_bin->event;
    g_autofree char *parent = NULL;
    int r;

    if (watch->source != NULL)
        return;

    r = sd_event_add_inotify(event,
                             &watch->source,
                             watch->dir,
                             PROGRAM_DIR_EVENTS,
                             mfr_host_bin_on_dir_changed,
                             watch);
    if (r >= 0) {
        g_clear_pointer(&watch->parent_source, sd_event_source_disable_unref);
        return;
    }
    if (r != -ENOENT && r != -ENOTDIR) {
        g_warning("Unable to watch %s: %s", watch->dir, g_strerror(-r));
        return;
    }
    if (watch->parent_source != NULL)
        return;

    parent = g_path_get_dirname(watch->dir);
    r = sd_event_add_inotify(event,
                             &watch->parent_source,
                             parent,
                             IN_CREATE | IN_MOVED_TO | IN_ONLYDIR,
                             mfr_host_bin_on_parent_changed,
                             watch);
    if (r < 0) {
        /* the parent directory is missing too, so we try again when the next sync runs */
        if (r != -ENOENT && r != -ENOTDIR)
            g_warning("Unable to watch %s: %s", parent, g_strerror(-r));
        return;
    }

    /* the directory may have been created just before we watched its parent */
    if (g_file_test(watch->dir, G_FILE_TEST_IS_DIR))
        mfr_host_bin_schedule_sync(watch->host_bin);
}

/**
 * mfr_host_bin_update_watches:
 */
static void
mfr_host_bin_update_watches(MfrHostBin *host_bin)
{
    for (guint i = 0; i < host_bin->watches->len; i++)
        mfr_host_bin_watch_update(&g_array_index(host_bin->watches, MfrHostBinWatch, i));
}

/**
 * mfr_host_bin_on_sync_timer:
 *
 * Called when the program directories of the host have changed, and the
 * delay has passed without a further change.
 */
static int
mfr_host_bin_on_sync_timer(sd_event_source *source, uint64_t usec, void *userdata)
{
    MfrHostBin *host_bin = userdata;
    g_autoptr(GError) error = NULL;

    /* we set up the watches first, so we do not miss a change that is made while we update the links */
    mfr_host_bin_update_watches(host_bin);
    if (!mfr_host_bin_sync(host_bin, &error))
        g_warning("Unable to update %s: %s", host_bin->links_dir, error->message);

    return 0;
}

/**
 * mfr_host_bin_start_watching:
 */
static gboolean
mfr_host_bin_start_watching(MfrHostBin *host_bin, GError **error)
{
    int r = sd_event_add_time_relative(host_bin->event,
                                       &host_bin->sync_timer,
                                       CLOCK_MONOTONIC,
                                       SYNC_DELAY_USEC,
                                       0,
                                       mfr_host_bin_on_sync_timer,
                                       host_bin);

    if (r >= 0)
        r = sd_event_source_set_enabled(host_bin->sync_timer, SD_EVENT_OFF);
    if (r < 0)
        return glnx_throw(error,
                          "Unable to set up the update of %s: %s",
                          host_bin->links_dir,
                          g_strerror(-r));

    /* NOTE: The watches are passed to callbacks, so the array must not grow after this. */
    for (char **dir = host_bin->program_dirs; *dir != NULL; dir++) {
        MfrHostBinWatch watch = {.host_bin = host_bin, .dir = *dir};

        g_array_append_val(host_bin->watches, watch);
    }
    mfr_host_bin_update_watches(host_bin);

    return TRUE;
}

/**
 * mfr_host_bin_new:
 * @event: (nullable): the event loop to watch the host in, or %NULL to not watch it
 * @links_dir: the directory to keep the links in
 * @program_dirs: (nullable) (array zero-terminated=1): the program directories of the host,
 *                or %NULL for the default directories
 * @error: a #GError
 *
 * Create the links for the programs of the host. If @event is set, the
 * links are updated whenever the programs of the host change.
 *
 * Returns: (transfer full): the links, or %NULL on error
 */
MfrHostBin *
mfr_host_bin_new(sd_event *event, const char *links_dir, const char *const *program_dirs, GError **error)
{
    g_autoptr(MfrHostBin) host_bin = NULL;

    g_return_val_if_fail(links_dir != NULL, NULL);

    host_bin = g_object_new(MFR_TYPE_HOST_BIN, NULL);
    host_bin->links_dir = g_strdup(links_dir);
    host_bin->program_dirs = g_strdupv(
        (char **) (program_dirs != NULL ? program_dirs : DEFAULT_PROGRAM_DIRS));

    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, links_dir, 0755, NULL, error))
        return NULL;

    /* we set up the watches first, so we do not miss a change that is made while we create the links */
    if (event != NULL) {
        host_bin->event = sd_event_ref(event);
        if (!mfr_host_bin_start_watching(host_bin, error))
            return NULL;
    }
    if (!mfr_host_bin_sync(host_bin, error))
        return NULL;

    return g_steal_pointer(&host_bin);
}

/**
 * mfr_host_bin_get_program_dirs:
 * @host_bin: the links
 *
 * Returns: (array zero-terminated=1): the program directories of the host that we create links for
 */
const char *const *
mfr_host_bin_get_program_dirs(MfrHostBin *host_bin)
{
    g_return_val_if_fail(MFR_IS_HOST_BIN(host_bin), NULL);
    return (const char *const *) host_bin->program_dirs;
}
