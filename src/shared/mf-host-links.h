/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Links to the programs of the host, shared by the compose daemon and the
 * runner.
 *
 * NOTE: This code must not use GLib, as the runner does not link to it.
 */

bool mf_dir_has_program(int         dir_fd,
                        const char *name);

int  mf_host_links_sync(int        links_fd,
                        const int *program_dir_fds,
                        size_t     n_program_dirs);
