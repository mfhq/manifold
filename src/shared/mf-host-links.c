/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include "config.h"
#include "mf-host-links.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mf-constants-private.h"

/* a growing list of file names */
typedef struct {
    char **names;
    size_t length;
    size_t allocated;
} MfNameList;

/**
 * mf_name_list_add:
 *
 * Returns: 0 on success, or a negative errno
 */
static int
mf_name_list_add(MfNameList *list, const char *name)
{
    if (list->length == list->allocated) {
        size_t allocated = list->allocated > 0 ? list->allocated * 2 : 256;
        char **names = reallocarray(list->names, allocated, sizeof(char *));

        if (names == nullptr)
            return -ENOMEM;
        list->names = names;
        list->allocated = allocated;
    }

    list->names[list->length] = strdup(name);
    if (list->names[list->length] == nullptr)
        return -ENOMEM;
    list->length++;
    return 0;
}

/**
 * mf_name_list_clear:
 */
static void
mf_name_list_clear(MfNameList *list)
{
    for (size_t i = 0; i < list->length; i++)
        free(list->names[i]);
    free(list->names);
    *list = (MfNameList){};
}

/**
 * mf_compare_names:
 *
 * Compare two entries of a #MfNameList, for qsort() and bsearch().
 */
static int
mf_compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/**
 * mf_name_list_sort:
 *
 * Sort the names, and remove the names that are listed more than once.
 */
static void
mf_name_list_sort(MfNameList *list)
{
    size_t kept = 0;

    if (list->length == 0)
        return;

    qsort(list->names, list->length, sizeof(char *), mf_compare_names);
    for (size_t i = 1; i < list->length; i++) {
        if (strcmp(list->names[kept], list->names[i]) == 0)
            free(list->names[i]);
        else
            list->names[++kept] = list->names[i];
    }
    list->length = kept + 1;
}

/**
 * mf_open_dir_stream:
 *
 * Open a stream for a directory that the caller has open. The stream has
 * a file descriptor of its own, so closing it does not close @dir_fd.
 *
 * NOTE: The stream shares its position with @dir_fd, so the caller must not
 * read from @dir_fd itself. We can not open the directory again, because a
 * user may be allowed to read a directory but not to search it.
 *
 * Returns: the stream, or nullptr with errno set
 */
static DIR *
mf_open_dir_stream(int dir_fd)
{
    int fd = fcntl(dir_fd, F_DUPFD_CLOEXEC, 3);
    DIR *dir;

    if (fd < 0)
        return nullptr;
    dir = fdopendir(fd);
    if (dir == nullptr) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return nullptr;
    }
    rewinddir(dir);
    return dir;
}

/**
 * mf_read_dir_stream:
 * @ret_entry: (out): the next entry, or nullptr at the end of the directory
 *
 * Read the next entry of a directory. readdir() returns nullptr both at
 * the end of a directory and if it fails, so we tell the two apart here.
 *
 * Returns: 0 on success, or a negative errno
 */
static int
mf_read_dir_stream(DIR *dir, struct dirent **ret_entry)
{
    errno = 0;
    *ret_entry = readdir(dir);
    return *ret_entry == nullptr && errno != 0 ? -errno : 0;
}

/**
 * mf_dir_has_program:
 * @dir_fd: a directory
 * @name: the name of a file in the directory
 *
 * Returns: true if the directory has an executable file with the given name
 */
bool
mf_dir_has_program(int dir_fd, const char *name)
{
    struct stat st;

    /* we follow links, so we accept a link to a program, and reject a broken link */
    return fstatat(dir_fd, name, &st, 0) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111) != 0;
}

/**
 * mf_host_links_collect_programs:
 *
 * Add the names of the programs in a directory to a list. Hidden files are
 * not added.
 *
 * Returns: 0 on success, or a negative errno
 */
static int
mf_host_links_collect_programs(int dir_fd, MfNameList *programs)
{
    DIR *dir = mf_open_dir_stream(dir_fd);
    struct dirent *entry;
    int r;

    if (dir == nullptr)
        return -errno;
    while ((r = mf_read_dir_stream(dir, &entry)) == 0 && entry != nullptr) {
        if (entry->d_name[0] != '.' && mf_dir_has_program(dir_fd, entry->d_name))
            r = mf_name_list_add(programs, entry->d_name);
        if (r < 0)
            break;
    }
    closedir(dir);

    return r;
}

/**
 * mf_host_links_sync:
 * @links_fd: the directory with the links
 * @program_dir_fds: (array length=n_program_dirs): the program directories of the host
 * @n_program_dirs: the number of program directories
 *
 * Update the links in a directory to match the programs in the given program
 * directories of the host. The directory gets one link to the trampoline
 * program for every program, see %MF_HOST_RUN_PATH. The links are updated in
 * place, because trees have the directory mounted.
 *
 * No link is changed if a program directory can not be read completely. A
 * link that can not be created or removed does not stop the update of the
 * other links.
 *
 * Returns: 0 on success, or the negative errno of the first failure
 */
int
mf_host_links_sync(int links_fd, const int *program_dir_fds, size_t n_program_dirs)
{
    MfNameList programs = {};
    bool *linked = nullptr;
    DIR *links = nullptr;
    struct dirent *entry;
    int read_result;
    int r = 0;

    for (size_t i = 0; i < n_program_dirs; i++) {
        r = mf_host_links_collect_programs(program_dir_fds[i], &programs);
        /* with an incomplete list, we would remove the links of programs that still exist */
        if (r < 0)
            goto out;
    }
    mf_name_list_sort(&programs);

    /* whether a program has a link already, in the order of the sorted names */
    linked = calloc(programs.length > 0 ? programs.length : 1, sizeof(bool));
    links = mf_open_dir_stream(links_fd);
    if (linked == nullptr || links == nullptr) {
        r = linked == nullptr ? -ENOMEM : -errno;
        goto out;
    }

    /* remove the links of programs that the host no longer has */
    while ((read_result = mf_read_dir_stream(links, &entry)) == 0 && entry != nullptr) {
        const char *name = entry->d_name;
        char **found;
        bool is_program = false;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        found = programs.length > 0
                    ? bsearch(&name, programs.names, programs.length, sizeof(char *), mf_compare_names)
                    : nullptr;
        if (found != nullptr) {
            linked[found - programs.names] = true;
            continue;
        }

        /* a program may have been added since we collected the names, and a different
         * process that runs at the same time may have created its link already */
        for (size_t i = 0; i < n_program_dirs && !is_program; i++)
            is_program = name[0] != '.' && mf_dir_has_program(program_dir_fds[i], name);
        if (!is_program && unlinkat(links_fd, name, 0) != 0 && errno != ENOENT && r == 0)
            r = -errno;
    }
    if (read_result < 0 && r == 0)
        r = read_result;

    for (size_t i = 0; i < programs.length; i++) {
        /* a different process may create the same link at the same time, so we ignore EEXIST */
        if (!linked[i] && symlinkat(MF_HOST_RUN_PATH, links_fd, programs.names[i]) != 0 && errno != EEXIST &&
            r == 0)
            r = -errno;
    }

out:
    if (links != nullptr)
        closedir(links);
    free(linked);
    mf_name_list_clear(&programs);
    return r;
}
