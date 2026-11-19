/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <manifold.h>

/*
 * Write the zchunk file of an image in the same way a repository does, and
 * print the size and the SHA-256 of the zchunk header, which are the values
 * that the component index records. The end-to-end tests use this tool to
 * create their test repositories.
 */

int
main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *header_sha256 = NULL;
    guint64 header_size;
    g_autofd int image_fd = -1;
    g_autofd int zck_fd = -1;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s IMAGE ZCHUNK-FILE\n", argv[0]);
        return 1;
    }

    image_fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    zck_fd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (image_fd < 0 || zck_fd < 0) {
        perror("open");
        return 1;
    }
    if (!mf_zchunk_write_image(image_fd, zck_fd, &error) ||
        !mf_zchunk_read_header(zck_fd, &header_size, &header_sha256, &error)) {
        fprintf(stderr, "%s\n", error->message);
        return 1;
    }

    printf("%" G_GUINT64_FORMAT " %s\n", header_size, header_sha256);
    return 0;
}
