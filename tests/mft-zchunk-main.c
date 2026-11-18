/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/*
 * Writes the zchunk file of an image the way repositories do, for the
 * repositories the end-to-end tests make up, and prints what the component
 * index records about its header: its size and its SHA-256.
 */

#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <manifold.h>

int
main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *header_sha256 = NULL;
    guint64 header_size;
    int image_fd;
    int zck_fd;

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
    close(image_fd);
    close(zck_fd);
    return 0;
}
