/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mff-downloader.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <gio/gio.h>

/* the maximum number of parallel transfers. With HTTP/2, they are multiplexed over a few connections. */
constexpr guint TRANSFERS_MAX = 16;

/* how many redirects we follow */
constexpr long REDIRECTS_MAX = 5;

/* we consider a transfer stalled if it stays below this speed for too long */
constexpr long LOW_SPEED_LIMIT = 1024;
constexpr long LOW_SPEED_TIME = 60;
constexpr long CONNECT_TIMEOUT = 30;

/* the HTTP status code for "Forbidden" */
constexpr long HTTP_FORBIDDEN = 403;

/**
 * mff_failure_kind_to_string:
 *
 * Returns: the name of @failure in the fetch interface
 */
const char *
mff_failure_kind_to_string(MffFailureKind failure)
{
    switch (failure) {
    case MFF_FAILURE_KIND_NETWORK:
        return "network";
    case MFF_FAILURE_KIND_HTTP:
        return "http";
    case MFF_FAILURE_KIND_TOO_LARGE:
        return "tooLarge";
    case MFF_FAILURE_KIND_BAD_RESPONSE:
        return "badResponse";
    case MFF_FAILURE_KIND_WRITE:
        return "write";
    case MFF_FAILURE_KIND_NONE:
    default:
        return NULL;
    }
}

/**
 * mff_request_new:
 * @fd: the file to write to, which is owned by the request from now on
 * @max_size: the largest size the file may have
 * @ranges: (nullable) (transfer full) (element-type MffRange): the ranges to fetch
 *
 * Returns: (transfer full): a new request
 */
MffRequest *
mff_request_new(const char *url, int fd, guint64 max_size, GArray *ranges)
{
    MffRequest *request = g_new0(MffRequest, 1);

    request->url = g_strdup(url);
    request->fd = fd;
    request->max_size = max_size;
    request->ranges = ranges;
    return request;
}

/**
 * mff_request_free:
 */
void
mff_request_free(MffRequest *request)
{
    if (request->fd >= 0)
        close(request->fd);
    g_free(request->url);
    g_clear_pointer(&request->ranges, g_array_unref);
    g_free(request->message);
    g_free(request);
}

/**
 * mff_request_fail:
 *
 * Record why a request failed. Only the first failure is recorded, and we do
 * not download the remaining parts of a failed request.
 */
static void
mff_request_fail(MffRequest *request, MffFailureKind failure, long http_status, const char *message)
{
    if (request->failure != MFF_FAILURE_KIND_NONE)
        return;
    request->failure = failure;
    request->http_status = http_status;
    request->message = g_strdup(message);
}

typedef struct _MffDownload MffDownload;

/**
 * MffJob:
 *
 * The state of downloading one request.
 */
typedef struct {
    MffRequest *request;
    char *location;   /* the URL that ranges are requested from, set once the first range was downloaded */
    guint next_range; /* the index of the first range that was not started yet */
    gboolean started; /* whether the first transfer of the request was started */
} MffJob;

/**
 * mff_job_free:
 */
static void
mff_job_free(MffJob *job)
{
    g_free(job->location);
    g_free(job);
}

/**
 * MffTransfer:
 *
 * A single HTTP request for a whole file or a range of a file.
 */
typedef struct {
    MffDownload *download;
    MffJob *job;
    CURL *easy;
    const MffRange *range; /* the range to fetch, owned by the request, or NULL for the whole file */
    guint64 written;
    gboolean redirected; /* whether the transfer uses the location of the job, not the URL of the request */
    gboolean retried;    /* whether this is the second attempt, which uses the URL of the request */
    gboolean checked;    /* whether we checked the reply headers already */
    char *content_range;
    char error_buffer[CURL_ERROR_SIZE];
} MffTransfer;

/* the state of downloading all requests of a call */
struct _MffDownload {
    CURLM *multi;
    const char *ca_file;
    GPtrArray *jobs;    /* (element-type MffJob) */
    GQueue retries;     /* of MffTransfer: second attempts that wait to be started */
    GPtrArray *running; /* (element-type MffTransfer) */
    guint64 received;   /* the number of bytes that all transfers have written */
};

/**
 * mff_transfer_free:
 */
static void
mff_transfer_free(MffTransfer *transfer)
{
    if (transfer->easy != NULL)
        curl_easy_cleanup(transfer->easy);
    g_free(transfer->content_range);
    g_free(transfer);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MffTransfer, mff_transfer_free)

/**
 * mff_transfer_check_reply:
 *
 * Check that the server replied with the range we asked for, or with the whole
 * file if we asked for that. Servers that do not support ranges reply with the
 * whole file, which we refuse.
 *
 * Returns: %FALSE if the reply is not what we asked for. The request is marked as failed then.
 */
static gboolean
mff_transfer_check_reply(MffTransfer *transfer)
{
    MffRequest *request = transfer->job->request;
    g_autofree char *expected = NULL;
    long status = 0;

    transfer->checked = TRUE;
    curl_easy_getinfo(transfer->easy, CURLINFO_RESPONSE_CODE, &status);
    if (transfer->range == NULL) {
        if (status == 200)
            return TRUE;
        mff_request_fail(request, MFF_FAILURE_KIND_BAD_RESPONSE, 0, "The server did not reply with the file");
        return FALSE;
    }

    expected = g_strdup_printf("bytes %" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT "/",
                               transfer->range->offset,
                               transfer->range->offset + transfer->range->length - 1);
    if (status == 206 && transfer->content_range != NULL &&
        g_str_has_prefix(transfer->content_range, expected))
        return TRUE;
    mff_request_fail(request,
                     MFF_FAILURE_KIND_BAD_RESPONSE,
                     0,
                     "The server did not reply with the range we asked for");
    return FALSE;
}

/**
 * mff_on_header:
 *
 * Remember the Content-Range of the last reply, which is the one after all redirects.
 */
static size_t
mff_on_header(char *data, size_t size, size_t n_items, void *user_data)
{
    MffTransfer *transfer = user_data;
    size_t len = size * n_items;
    g_autofree char *line = g_strndup(data, len);

    if (g_str_has_prefix(line, "HTTP/")) {
        g_clear_pointer(&transfer->content_range, g_free);
    } else if (g_ascii_strncasecmp(line, "Content-Range:", strlen("Content-Range:")) == 0) {
        g_free(transfer->content_range);
        transfer->content_range = g_strdup(g_strstrip(line + strlen("Content-Range:")));
    }

    return len;
}

/**
 * mff_on_data:
 *
 * Write received data at the right offset in the file. We never write past
 * the end of the range or past the maximum size of the request.
 *
 * Returns: the number of bytes written, or 0 to stop the transfer because its
 *   request has failed
 */
static size_t
mff_on_data(char *data, size_t size, size_t n_items, void *user_data)
{
    MffTransfer *transfer = user_data;
    MffRequest *request = transfer->job->request;
    size_t len = size * n_items;
    guint64 offset = transfer->range != NULL ? transfer->range->offset : 0;
    guint64 limit = transfer->range != NULL ? transfer->range->length : request->max_size;

    /* we check a reply when its first data arrives */
    if (!transfer->checked && !mff_transfer_check_reply(transfer))
        return 0;
    if (len > limit - transfer->written) {
        if (transfer->range != NULL)
            mff_request_fail(request,
                             MFF_FAILURE_KIND_BAD_RESPONSE,
                             0,
                             "The server sent more than the range we asked for");
        else
            mff_request_fail(request, MFF_FAILURE_KIND_TOO_LARGE, 0, "The file is larger than it may be");
        return 0;
    }

    for (size_t done = 0; done < len;) {
        ssize_t n = pwrite(request->fd, data + done, len - done, (off_t) (offset + transfer->written + done));

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            mff_request_fail(request,
                             MFF_FAILURE_KIND_WRITE,
                             0,
                             n < 0 ? g_strerror(errno) : "The file does not take more data");
            return 0;
        }
        done += (size_t) n;
    }
    transfer->written += len;
    request->size += len;
    transfer->download->received += len;

    return len;
}

/**
 * mff_transfer_new:
 * @range: (nullable): the range to fetch, or %NULL for the whole file
 * @retry: %TRUE if this is the second attempt for a range. It uses the URL of
 *   the request, because the location we were redirected to has expired.
 *
 * Returns: (transfer full) (nullable): a transfer that was not started yet
 */
static MffTransfer *
mff_transfer_new(MffDownload *download, MffJob *job, const MffRange *range, gboolean retry)
{
    g_autoptr(MffTransfer) transfer = g_new0(MffTransfer, 1);
    MffRequest *request = job->request;
    g_autofree char *range_value = NULL;

    transfer->download = download;
    transfer->job = job;
    transfer->range = range;
    transfer->retried = retry;
    transfer->redirected = !retry && job->location != NULL && !g_str_equal(job->location, request->url);
    transfer->easy = curl_easy_init();
    if (transfer->easy == NULL)
        return NULL;

    curl_easy_setopt(transfer->easy, CURLOPT_URL, transfer->redirected ? job->location : request->url);
    /* we must never download via anything but https, so we fail if these options can not be set */
    if (curl_easy_setopt(transfer->easy, CURLOPT_PROTOCOLS_STR, "https") != CURLE_OK ||
        curl_easy_setopt(transfer->easy, CURLOPT_REDIR_PROTOCOLS_STR, "https") != CURLE_OK)
        return NULL;
    curl_easy_setopt(transfer->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(transfer->easy, CURLOPT_MAXREDIRS, REDIRECTS_MAX);
    curl_easy_setopt(transfer->easy, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(transfer->easy, CURLOPT_USERAGENT, "manifold/" MF_VERSION);
    curl_easy_setopt(transfer->easy, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT);
    curl_easy_setopt(transfer->easy, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);
    curl_easy_setopt(transfer->easy, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME);
    /* prefer waiting for a connection we can reuse over opening a new one */
    curl_easy_setopt(transfer->easy, CURLOPT_PIPEWAIT, 1L);
    curl_easy_setopt(transfer->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(transfer->easy, CURLOPT_ERRORBUFFER, transfer->error_buffer);
    curl_easy_setopt(transfer->easy, CURLOPT_HEADERFUNCTION, mff_on_header);
    curl_easy_setopt(transfer->easy, CURLOPT_HEADERDATA, transfer);
    curl_easy_setopt(transfer->easy, CURLOPT_WRITEFUNCTION, mff_on_data);
    curl_easy_setopt(transfer->easy, CURLOPT_WRITEDATA, transfer);
    curl_easy_setopt(transfer->easy, CURLOPT_PRIVATE, transfer);
    if (download->ca_file != NULL)
        curl_easy_setopt(transfer->easy, CURLOPT_CAINFO, download->ca_file);

    if (range != NULL) {
        range_value = g_strdup_printf("%" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT,
                                      range->offset,
                                      range->offset + range->length - 1);
        curl_easy_setopt(transfer->easy, CURLOPT_RANGE, range_value);
    } else {
        curl_easy_setopt(transfer->easy, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t) request->max_size);
    }

    return g_steal_pointer(&transfer);
}

/**
 * mff_job_next_transfer:
 * @ret_transfer: (out) (transfer full) (nullable): the next transfer of the
 *   job, or %NULL if the job has none to start right now
 *
 * Create the next transfer of a job. The first range of a request is
 * downloaded on its own, to find the location that the URL of the request
 * redirects to. All other ranges are requested from that location directly,
 * so the redirects are only followed once.
 *
 * Returns: %FALSE if a transfer could not be set up
 */
static gboolean
mff_job_next_transfer(MffDownload *download, MffJob *job, MffTransfer **ret_transfer)
{
    MffRequest *request = job->request;
    const MffRange *range = NULL;

    *ret_transfer = NULL;
    if (request->failure != MFF_FAILURE_KIND_NONE)
        return TRUE;
    if (job->started) {
        /* the other ranges can only start once the first range is downloaded */
        if (request->ranges == NULL || job->location == NULL || job->next_range >= request->ranges->len)
            return TRUE;
    }

    if (request->ranges != NULL)
        range = &g_array_index(request->ranges, MffRange, job->next_range++);
    job->started = TRUE;
    *ret_transfer = mff_transfer_new(download, job, range, FALSE);
    return *ret_transfer != NULL;
}

/**
 * mff_transfer_needs_retry:
 *
 * The temporary location that a request was redirected to may expire while
 * we download its ranges. The server then replies with "Forbidden" to a
 * transfer that uses the location, and we try the URL of the request again.
 *
 * Returns: %TRUE if the failed transfer should be tried a second time
 */
static gboolean
mff_transfer_needs_retry(MffTransfer *transfer, CURLcode code, long http_status)
{
    return code == CURLE_HTTP_RETURNED_ERROR && http_status == HTTP_FORBIDDEN && transfer->redirected &&
           !transfer->retried;
}

/**
 * mff_transfer_fail_request:
 *
 * Mark the request of a transfer as failed, with the reason that libcurl gives for the failure.
 */
static void
mff_transfer_fail_request(MffTransfer *transfer, CURLcode code, long http_status)
{
    MffRequest *request = transfer->job->request;
    const char *message = transfer->error_buffer[0] != '\0' ? transfer->error_buffer
                                                            : curl_easy_strerror(code);

    switch (code) {
    case CURLE_HTTP_RETURNED_ERROR:
        mff_request_fail(request, MFF_FAILURE_KIND_HTTP, http_status, message);
        break;
    case CURLE_FILESIZE_EXCEEDED:
        mff_request_fail(request, MFF_FAILURE_KIND_TOO_LARGE, 0, "The file is larger than it may be");
        break;
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_TOO_MANY_REDIRECTS:
        mff_request_fail(request, MFF_FAILURE_KIND_BAD_RESPONSE, 0, message);
        break;
    default:
        /* NOTE: if one of our callbacks stopped the transfer, it has recorded the failure already */
        mff_request_fail(request, MFF_FAILURE_KIND_NETWORK, 0, message);
        break;
    }
}

/**
 * mff_download_finish:
 *
 * Process a finished transfer: check that it wrote what we asked for, or
 * record why it failed.
 *
 * Returns: %FALSE if a retry could not be set up
 */
static gboolean
mff_download_finish(MffDownload *download, MffTransfer *transfer, CURLcode code)
{
    MffJob *job = transfer->job;
    MffRequest *request = job->request;
    const char *location = NULL;
    long http_status = 0;

    curl_easy_getinfo(transfer->easy, CURLINFO_RESPONSE_CODE, &http_status);
    if (code != CURLE_OK) {
        if (mff_transfer_needs_retry(transfer, code, http_status)) {
            MffTransfer *retry = mff_transfer_new(download, job, transfer->range, TRUE);

            if (retry == NULL)
                return FALSE;
            g_queue_push_tail(&download->retries, retry);
            return TRUE;
        }
        mff_transfer_fail_request(transfer, code, http_status);
        return TRUE;
    }

    /* a reply without data was not checked yet */
    if (!transfer->checked && !mff_transfer_check_reply(transfer))
        return TRUE;
    if (transfer->range == NULL)
        return TRUE;
    if (transfer->written != transfer->range->length) {
        mff_request_fail(request,
                         MFF_FAILURE_KIND_BAD_RESPONSE,
                         0,
                         "The server sent less than the range we asked for");
        return TRUE;
    }

    /* the first range and every second attempt use the URL of the request, so they tell
     * us where the URL redirects to at the moment. We request the next ranges from there. */
    if (job->location == NULL || transfer->retried) {
        curl_easy_getinfo(transfer->easy, CURLINFO_EFFECTIVE_URL, &location);
        g_free(job->location);
        job->location = g_strdup(location != NULL ? location : request->url);
    }

    return TRUE;
}

/**
 * mff_download_start_transfers:
 *
 * Start transfers until the maximum number of parallel transfers is running.
 * Second attempts are started first, then the next transfer of each job.
 */
static gboolean
mff_download_start_transfers(MffDownload *download)
{
    for (guint i = 0; download->running->len < TRANSFERS_MAX;) {
        MffTransfer *transfer = g_queue_pop_head(&download->retries);

        /* take one transfer from each job in turn, until no job has a transfer to start */
        for (guint n_idle = 0; transfer == NULL && n_idle < download->jobs->len; n_idle++) {
            MffJob *job = g_ptr_array_index(download->jobs, i);

            i = (i + 1) % download->jobs->len;
            if (!mff_job_next_transfer(download, job, &transfer))
                return FALSE;
        }
        if (transfer == NULL)
            break;

        if (transfer->job->request->failure != MFF_FAILURE_KIND_NONE) {
            mff_transfer_free(transfer);
            continue;
        }
        if (curl_multi_add_handle(download->multi, transfer->easy) != CURLM_OK) {
            mff_transfer_free(transfer);
            return FALSE;
        }
        g_ptr_array_add(download->running, transfer);
    }
    return TRUE;
}

/**
 * mff_download:
 * @requests: (element-type MffRequest): the requests to download
 * @ca_file: (nullable): the only certificate authority we trust, or %NULL to use the system's
 * @progress_func: (nullable): called regularly, and can stop the download
 * @error: a #GError
 *
 * Download all requests in parallel. A failed request records the reason for
 * its failure, and does not stop the other requests.
 *
 * Returns: %FALSE if the download could not be run, or was stopped
 */
gboolean
mff_download(GPtrArray *requests,
             const char *ca_file,
             MffProgressFunc progress_func,
             gpointer user_data,
             GError **error)
{
    MffDownload download = {
        .ca_file = ca_file,
        .jobs = g_ptr_array_new_with_free_func((GDestroyNotify) mff_job_free),
        .running = g_ptr_array_new(),
    };
    gboolean cancelled = FALSE;
    gboolean ret = FALSE;

    download.multi = curl_multi_init();
    if (download.multi == NULL)
        goto out;
    curl_multi_setopt(download.multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

    for (guint i = 0; i < requests->len; i++) {
        MffJob *job = g_new0(MffJob, 1);

        job->request = g_ptr_array_index(requests, i);
        g_ptr_array_add(download.jobs, job);
    }

    for (;;) {
        CURLMsg *msg;
        int msgs_left;
        int still_running;

        if (!mff_download_start_transfers(&download))
            goto out;
        if (download.running->len == 0)
            break;
        if (curl_multi_perform(download.multi, &still_running) != CURLM_OK)
            goto out;

        while ((msg = curl_multi_info_read(download.multi, &msgs_left)) != NULL) {
            g_autoptr(MffTransfer) transfer = NULL;
            CURLcode code = msg->data.result;

            if (msg->msg != CURLMSG_DONE)
                continue;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char **) &transfer);
            /* NOTE: the message is not valid anymore once its handle is removed */
            curl_multi_remove_handle(download.multi, transfer->easy);
            g_ptr_array_remove_fast(download.running, transfer);
            if (!mff_download_finish(&download, transfer, code))
                goto out;
        }

        if (progress_func != NULL && !progress_func(download.received, user_data)) {
            cancelled = TRUE;
            goto out;
        }
        if (download.running->len > 0 && curl_multi_poll(download.multi, NULL, 0, 1000, NULL) != CURLM_OK)
            goto out;
    }

    ret = TRUE;

out:
    if (cancelled)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "The download was stopped");
    else if (!ret)
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            download.multi == NULL ? "Unable to set up libcurl"
                                                   : "Unable to run the downloads");
    for (guint i = 0; i < download.running->len; i++) {
        MffTransfer *transfer = g_ptr_array_index(download.running, i);

        curl_multi_remove_handle(download.multi, transfer->easy);
        mff_transfer_free(transfer);
    }
    g_ptr_array_unref(download.running);
    g_queue_clear_full(&download.retries, (GDestroyNotify) mff_transfer_free);
    g_ptr_array_unref(download.jobs);
    if (download.multi != NULL)
        curl_multi_cleanup(download.multi);
    return ret;
}
