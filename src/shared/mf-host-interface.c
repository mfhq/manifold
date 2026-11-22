/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/*
 * Definition of the org.freedesktop.manifold.host Varlink interface, which
 * applications use to run a program on the host.
 *
 * NOTE: The client of this interface is part of the core images. A core that is
 * installed keeps its client, so the interface may only be extended in ways
 * that an old client still works with.
 */

#define _GNU_SOURCE
#include "mf-host-interface.h"

/* clang-format off */

static SD_VARLINK_DEFINE_METHOD_FULL(
    Run,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("The program to run and its arguments. A program that is given by its name "
                             "is searched in the PATH of the host."),
    SD_VARLINK_DEFINE_INPUT(argv, SD_VARLINK_STRING, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("Absolute path of the directory to run the program in. "
                             "It has to be a directory of the host."),
    SD_VARLINK_DEFINE_INPUT(workingDirectory, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The environment of the caller, as NAME=VALUE. The service sets these variables "
                             "over the environment of the session of the user, except for the ones that "
                             "only make sense inside of the mount tree of an application."),
    SD_VARLINK_DEFINE_INPUT(environment, SD_VARLINK_STRING, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("Indices of the file descriptors that are passed along with the call. The first "
                             "one becomes file descriptor 0 of the program, the second one file descriptor 1, "
                             "and so on. An index may be given more than once. Standard input, output and error "
                             "are /dev/null if they are not given."),
    SD_VARLINK_DEFINE_INPUT(fileDescriptors, SD_VARLINK_INT, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("Index of a file descriptor of the working directory. The service uses it to "
                             "check that workingDirectory is the same directory on the host."),
    SD_VARLINK_DEFINE_INPUT(workingDirectoryFileDescriptor, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("Index of the file descriptor of the read end of a pipe. The caller writes "
                             "signal numbers to the pipe, one byte each, and the service sends these "
                             "signals to the program."),
    SD_VARLINK_DEFINE_INPUT(signalFileDescriptor, SD_VARLINK_INT, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The process ID of the program on the host. Only set in the first reply of a "
                             "call with the 'more' flag, which is sent when the program was started."),
    SD_VARLINK_DEFINE_OUTPUT(pid, SD_VARLINK_INT, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The exit status of the program, if it exited."),
    SD_VARLINK_DEFINE_OUTPUT(exitStatus, SD_VARLINK_INT, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The number of the signal that ended the program, if it was ended by one."),
    SD_VARLINK_DEFINE_OUTPUT(signal, SD_VARLINK_INT, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_ERROR(
    NotPermitted,
    SD_VARLINK_FIELD_COMMENT("Identifier of the application that called."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    NoSuchProgram,
    SD_VARLINK_FIELD_COMMENT("The program that was not found."),
    SD_VARLINK_DEFINE_FIELD(program, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    NoSuchWorkingDirectory,
    SD_VARLINK_FIELD_COMMENT("The requested working directory."),
    SD_VARLINK_DEFINE_FIELD(path, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    SpawnFailed,
    SD_VARLINK_FIELD_COMMENT("Description of the problem."),
    SD_VARLINK_DEFINE_FIELD(message, SD_VARLINK_STRING, 0));

SD_VARLINK_DEFINE_INTERFACE(
    org_freedesktop_manifold_host,
    MF_HOST_INTERFACE,
    SD_VARLINK_INTERFACE_COMMENT("Runs programs of the host for Manifold applications, as the user and in "
                                 "the session of the user."),

    SD_VARLINK_SYMBOL_COMMENT("Run a program on the host. The reply is sent when the program has ended. "
                              "The program is ended if the caller closes the connection before that."),
    &vl_method_Run,

    SD_VARLINK_SYMBOL_COMMENT("The application that called does not have the host-commands permission."),
    &vl_error_NotPermitted,
    SD_VARLINK_SYMBOL_COMMENT("The host does not have the program."),
    &vl_error_NoSuchProgram,
    SD_VARLINK_SYMBOL_COMMENT("The working directory of the caller is not a directory of the host. This is the "
                              "case for a directory that only exists in the mount tree of the application."),
    &vl_error_NoSuchWorkingDirectory,
    SD_VARLINK_SYMBOL_COMMENT("The program could not be started."),
    &vl_error_SpawnFailed);

/* clang-format on */
