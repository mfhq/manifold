/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-progress-tracker.h"

#include "mf-progress-private.h"

/**
 * mfd_progress_tracker_clear:
 */
void
mfd_progress_tracker_clear(MfdProgressTracker *tracker)
{
    g_clear_pointer(&tracker->progress, mf_progress_free);
    tracker->item = NULL;
}

/* the number of bytes of an image that we copy between two progress reports */
constexpr uint64_t PROGRESS_STEP_BYTES = 4 * 1024 * 1024;

/**
 * mfd_progress_report:
 *
 * Report the current progress of the operation to the caller.
 */
static void
mfd_progress_report(MfdProgressTracker *tracker)
{
    double item_percent = 0;

    if (tracker->func == NULL)
        return;

    if (tracker->item != NULL) {
        uint64_t bytes_done = mf_item_progress_get_bytes_done(tracker->item);
        uint64_t bytes_total = mf_item_progress_get_bytes_total(tracker->item);

        if (bytes_total > 0)
            item_percent = (double) bytes_done * 100 / (double) bytes_total;
        else if (tracker->item_done)
            item_percent = 100;
        mf_item_progress_set_percent(tracker->item, item_percent);
        tracker->bytes_last_reported = bytes_done;
    }
    double percent = tracker->total > 0
                         ? ((double) tracker->done * 100 + item_percent) / (double) tracker->total
                         : 100;
    mf_progress_set_percent(tracker->progress, MIN(percent, 100));

    tracker->func(tracker->progress, tracker->user_data);
}

/**
 * mfd_progress_begin_item:
 * @version: (nullable): the version of the component that the item is about
 * @bytes_total: the size of the image the item copies, or 0
 *
 * Report that work on an item begins.
 */
void
mfd_progress_begin_item(MfdProgressTracker *tracker,
                        MfAction action,
                        const char *subject,
                        const char *version,
                        uint64_t bytes_total)
{
    tracker->item = mf_item_progress_new(action, subject, version);
    mf_item_progress_set_bytes_total(tracker->item, bytes_total);
    mf_progress_take_item(tracker->progress, tracker->item);
    tracker->bytes_last_reported = 0;
    tracker->item_done = FALSE;
    mfd_progress_report(tracker);
}

/**
 * mfd_progress_end_item:
 *
 * Report that the item is done.
 */
void
mfd_progress_end_item(MfdProgressTracker *tracker)
{
    mf_item_progress_set_bytes_done(tracker->item, mf_item_progress_get_bytes_total(tracker->item));
    tracker->item_done = TRUE;
    mfd_progress_report(tracker);
    mf_progress_take_item(tracker->progress, NULL);
    tracker->item = NULL;
    tracker->done++;
}

/**
 * mfd_progress_skip_item:
 *
 * Count an item as done, without reporting it as an item of its own.
 */
void
mfd_progress_skip_item(MfdProgressTracker *tracker)
{
    tracker->done++;
    mfd_progress_report(tracker);
}

/**
 * mfd_progress_split_last_item:
 * @n_items: the number of items to split the last item into
 *
 * Split the last item of the operation into @n_items items, which together
 * take the share of the progress that the last item had. If @n_items is zero,
 * the last item is counted as done right away.
 */
void
mfd_progress_split_last_item(MfdProgressTracker *tracker, guint n_items)
{
    if (n_items == 0) {
        tracker->done = tracker->total;
        mfd_progress_report(tracker);
        return;
    }
    tracker->total *= n_items;
    tracker->done *= n_items;
}

/**
 * mfd_progress_on_copy:
 *
 * Report the progress of writing an image into the store.
 */
void
mfd_progress_on_copy(uint64_t bytes_copied, gpointer user_data)
{
    MfdProgressTracker *tracker = user_data;

    mf_item_progress_set_bytes_done(tracker->item, bytes_copied);
    /* the last bytes are reported when the item ends */
    if (bytes_copied - tracker->bytes_last_reported < PROGRESS_STEP_BYTES ||
        bytes_copied == mf_item_progress_get_bytes_total(tracker->item))
        return;
    mfd_progress_report(tracker);
}
