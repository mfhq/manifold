/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/*
 * Definition of the org.freedesktop.manifold.fetch Varlink interface, which
 * only manifoldd uses, to talk to its download helper.
 */

#define _GNU_SOURCE
#include "mf-fetch-interface.h"

/* clang-format off */

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Range,
    SD_VARLINK_FIELD_COMMENT("Where the range starts, in bytes from the start of the file."),
    SD_VARLINK_DEFINE_FIELD(offset, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("The number of bytes of the range, at least one."),
    SD_VARLINK_DEFINE_FIELD(length, SD_VARLINK_INT, 0));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Request,
    SD_VARLINK_FIELD_COMMENT("The https URL of the file."),
    SD_VARLINK_DEFINE_FIELD(url, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The index of the file descriptor to write to, which has to be seekable."),
    SD_VARLINK_DEFINE_FIELD(fileDescriptor, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("The largest the file may be. Nothing is written at or after this offset."),
    SD_VARLINK_DEFINE_FIELD(maxSize, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("The ranges of the file to fetch, each written at its offset. "
                             "Without them, the whole file is written from the start."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(ranges, Range, SD_VARLINK_ARRAY | SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_ENUM_TYPE(
    Failure,
    SD_VARLINK_FIELD_COMMENT("The server could not be reached, or the connection broke."),
    SD_VARLINK_DEFINE_ENUM_VALUE(network),
    SD_VARLINK_FIELD_COMMENT("The server replied with an HTTP error."),
    SD_VARLINK_DEFINE_ENUM_VALUE(http),
    SD_VARLINK_FIELD_COMMENT("The file is larger than its maxSize."),
    SD_VARLINK_DEFINE_ENUM_VALUE(tooLarge),
    SD_VARLINK_FIELD_COMMENT("The server did not reply with what was asked for, for example with the whole "
                             "file instead of a range, or it redirected to a URL that is not https."),
    SD_VARLINK_DEFINE_ENUM_VALUE(badResponse),
    SD_VARLINK_FIELD_COMMENT("Writing to the file descriptor failed."),
    SD_VARLINK_DEFINE_ENUM_VALUE(write));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Result,
    SD_VARLINK_FIELD_COMMENT("The number of bytes written."),
    SD_VARLINK_DEFINE_FIELD(size, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("Why the request failed, or null if it succeeded."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(failure, Failure, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The HTTP status of the reply that failed, if there was one."),
    SD_VARLINK_DEFINE_FIELD(httpStatus, SD_VARLINK_INT, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("What went wrong, in English."),
    SD_VARLINK_DEFINE_FIELD(message, SD_VARLINK_STRING, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_METHOD_FULL(
    Fetch,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("The files, or ranges of files, to download."),
    SD_VARLINK_DEFINE_INPUT_BY_TYPE(requests, Request, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("The number of bytes received so far, for all requests together."),
    SD_VARLINK_DEFINE_OUTPUT(received, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("The result of each request, in the order of the requests. "
                             "Only the last reply has them."),
    SD_VARLINK_DEFINE_OUTPUT_BY_TYPE(results, Result, SD_VARLINK_ARRAY | SD_VARLINK_NULLABLE));

SD_VARLINK_DEFINE_INTERFACE(
    org_freedesktop_manifold_fetch,
    MF_FETCH_INTERFACE,
    SD_VARLINK_INTERFACE_COMMENT("Downloads files over https for manifoldd, into files that manifoldd opened."),

    SD_VARLINK_SYMBOL_COMMENT("A range of bytes of a file."),
    &vl_type_Range,
    SD_VARLINK_SYMBOL_COMMENT("A file to download."),
    &vl_type_Request,
    SD_VARLINK_SYMBOL_COMMENT("Why a request failed."),
    &vl_type_Failure,
    SD_VARLINK_SYMBOL_COMMENT("The result of a request."),
    &vl_type_Result,

    SD_VARLINK_SYMBOL_COMMENT("Download files, or ranges of them, all at once. A request that fails does not "
                              "stop the others. With the 'more' flag, the progress is reported on the way."),
    &vl_method_Fetch);

/* clang-format on */
