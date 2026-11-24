/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdint.h>
#include <glib.h>

#include <manifold.h>

G_BEGIN_DECLS

/**
 * MfdProgressTracker:
 *
 * Progress state of the current operation: the #MfProgress that is
 * reported, and the counters that it is calculated from.
 */
typedef struct {
    MfProgressFunc  func;
    gpointer        user_data;
    MfProgress     *progress;
    MfItemProgress *item;  /* the item being worked on, owned by @progress */
    guint           total; /* number of items of the operation */
    guint           done;  /* number of finished items */
    uint64_t        bytes_last_reported;
    gboolean        item_done;
} MfdProgressTracker;

void mfd_progress_tracker_clear(MfdProgressTracker *tracker);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfdProgressTracker,
                                 mfd_progress_tracker_clear)

void mfd_progress_begin_item(MfdProgressTracker *tracker,
                             MfAction            action,
                             const char         *subject,
                             const char         *version,
                             uint64_t            bytes_total);
void mfd_progress_end_item(MfdProgressTracker *tracker);
void mfd_progress_skip_item(MfdProgressTracker *tracker);
void mfd_progress_split_last_item(MfdProgressTracker *tracker,
                                  guint               n_items);
void mfd_progress_on_copy(uint64_t bytes_copied,
                          gpointer user_data);

G_END_DECLS
