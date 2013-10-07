/* librepo - A library providing (libcURL like) API to downloading repository
 * Copyright (C) 2012  Tomas Mlcoch
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#define _GNU_SOURCE  // for GNU basename() implementation from string.h
#include <glib.h>
#include <glib/gstdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "types.h"
#include "util.h"
#include "package_downloader.h"
#include "handle_internal.h"
#include "downloader.h"

/* Do NOT use resume on successfully downloaded files - download will fail */

LrPackageTarget *
lr_packagetarget_new(const char *relative_url,
                     const char *dest,
                     LrChecksumType checksum_type,
                     const char *checksum,
                     gint64 expectedsize,
                     const char *base_url,
                     gboolean resume,
                     LrProgressCb progresscb,
                     void *cbdata,
                     GError **err)
{
    LrPackageTarget *target;

    assert(relative_url);
    assert(!err || *err == NULL);

    target = lr_malloc0(sizeof(*target));
    if (!target) {
        g_set_error(err, LR_PACKAGE_DOWNLOADER_ERROR, LRE_MEMORY,
                    "Out of memory");
        return NULL;
    }

    target->chunk = g_string_chunk_new(16);

    target->relative_url = lr_string_chunk_insert(target->chunk, relative_url);
    target->dest = lr_string_chunk_insert(target->chunk, dest);
    target->checksum_type = checksum_type;
    target->checksum = lr_string_chunk_insert(target->chunk, checksum);
    target->expectedsize = expectedsize;
    target->base_url = lr_string_chunk_insert(target->chunk, base_url);
    target->resume = resume;
    target->progresscb = progresscb;
    target->cbdata = cbdata;

    return target;
}

void
lr_packagetarget_free(LrPackageTarget *target)
{
    g_string_chunk_free(target->chunk);
    g_free(target);
}

gboolean
lr_download_packages(LrHandle *handle,
                     GSList *targets,
                     LrPackageDownloadFlag flags,
                     GError **err)
{
    gboolean ret;
    gboolean failfast = flags & LR_PACKAGEDOWNLOAD_FAILFAST;
    struct sigaction old_sigact;
    GSList *downloadtargets = NULL;

    assert(handle);
    assert(!err || *err == NULL);

    if (!targets)
        return TRUE;

    // Check repotype
    if (handle->repotype != LR_YUMREPO) {
        g_debug("%s: Bad repo type", __func__);
        assert(0);
        g_set_error(err, LR_PACKAGE_DOWNLOADER_ERROR, LRE_BADFUNCARG,
                    "Bad repo type");
        return FALSE;
    }

    // Setup sighandler
    if (handle->interruptible) {
        g_debug("%s: Using own SIGINT handler", __func__);
        struct sigaction sigact;
        sigact.sa_handler = lr_sigint_handler;
        sigaddset(&sigact.sa_mask, SIGINT);
        sigact.sa_flags = SA_RESTART;
        if (sigaction(SIGINT, &sigact, &old_sigact) == -1) {
            g_set_error(err, LR_PACKAGE_DOWNLOADER_ERROR, LRE_SIGACTION,
                        "Cannot set Librepo SIGINT handler");
            return FALSE;
        }
    }

    // Prepare internal mirrorlist
    ret = lr_handle_prepare_internal_mirrorlist(handle, err);
    if (!ret)
        goto cleanup;

    // Prepare targets
    for (GSList *elem = targets; elem; elem = g_slist_next(elem)) {
        gchar *local_path;
        LrPackageTarget *packagetarget = elem->data;
        LrDownloadTarget *downloadtarget;

        // Prepare destination filename
        if (packagetarget->dest) {
            if (g_file_test(packagetarget->dest, G_FILE_TEST_IS_DIR)) {
                // Dir specified
                gchar *file_basename = g_path_get_basename(packagetarget->relative_url);
                local_path = g_build_filename(packagetarget->dest,
                                              file_basename,
                                              NULL);
                g_free(file_basename);
            } else {
                local_path = g_strdup(packagetarget->dest);
            }
        } else {
            // No destination path specified
            local_path = g_path_get_basename(packagetarget->relative_url);
        }

        packagetarget->local_path = g_string_chunk_insert(packagetarget->chunk,
                                                          local_path);
        g_free(local_path);

        // If the file exists and checksum is passed, check if we need to
        // download the file again
        if (g_access(packagetarget->local_path, R_OK) == 0
            && packagetarget->checksum
            && packagetarget->checksum_type != LR_CHECKSUM_UNKNOWN)
        {
            /* If the file exists and checksum is ok, then is pointless to
             * download the file again.
             * Moreover, if the resume is enabled and the file is already
             * completely downloaded, then the download is going to fail.
             */
            int fd_r = open(packagetarget->local_path, O_RDONLY);
            if (fd_r != -1) {
                gboolean matches;
                ret = lr_checksum_fd_cmp(packagetarget->checksum_type,
                                         fd_r,
                                         packagetarget->checksum,
                                         0,
                                         &matches,
                                         NULL);
                close(fd_r);
                if (ret && matches) {
                    g_debug("%s: Package %s is already downloaded (checksum matches)",
                            __func__, packagetarget->local_path);
                    packagetarget->err = g_string_chunk_insert(packagetarget->chunk,
                                         "Already downloaded");
                    continue;
                }
            }
        }

        downloadtarget = lr_downloadtarget_new(packagetarget->relative_url,
                                               packagetarget->base_url,
                                               -1,
                                               packagetarget->local_path,
                                               packagetarget->checksum_type,
                                               packagetarget->checksum,
                                               packagetarget->expectedsize,
                                               packagetarget->resume,
                                               packagetarget->progresscb,
                                               packagetarget->cbdata,
                                               packagetarget);

        downloadtargets = g_slist_append(downloadtargets, downloadtarget);
    }

    // Start downloading
    ret = lr_download(handle, downloadtargets, failfast, err);

cleanup:

    // Copy download statuses from downloadtargets to targets
    for (GSList *elem = downloadtargets; elem; elem = g_slist_next(elem)) {
        LrDownloadTarget *downloadtarget = elem->data;
        LrPackageTarget *packagetarget = downloadtarget->userdata;
        if (downloadtarget->err)
            packagetarget->err = g_string_chunk_insert(packagetarget->chunk,
                                                       downloadtarget->err);
    }

    // Free downloadtargets list
    g_slist_free_full(downloadtargets, (GDestroyNotify)lr_downloadtarget_free);

    // Restore original signal handler
    if (handle->interruptible) {
        g_debug("%s: Restoring an old SIGINT handler", __func__);
        sigaction(SIGINT, &old_sigact, NULL);
        if (lr_interrupt) {
            if (err && *err != NULL)
                g_clear_error(err);
            g_set_error(err, LR_PACKAGE_DOWNLOADER_ERROR, LRE_INTERRUPTED,
                        "Insterupted by a SIGINT signal");
            return FALSE;
        }
    }

    return ret;
}

gboolean
lr_download_package(LrHandle *handle,
                    const char *relative_url,
                    const char *dest,
                    LrChecksumType checksum_type,
                    const char *checksum,
                    gint64 expectedsize,
                    const char *base_url,
                    gboolean resume,
                    GError **err)
{
    LrPackageTarget *target;

    assert(handle);
    assert(!err || *err == NULL);

    // XXX: Maybe remove in future
    if (!dest)
        dest = handle->destdir;

    // XXX: Maybe remove usage of handle callback in future

    target = lr_packagetarget_new(relative_url, dest, checksum_type, checksum,
                                  expectedsize, base_url, resume,
                                  handle->user_cb, handle->user_data, err);
    if (!target)
        return FALSE;

    GSList *targets = NULL;
    targets = g_slist_append(targets, target);

    gboolean ret = lr_download_packages(handle,
                                        targets,
                                        LR_PACKAGEDOWNLOAD_FAILFAST,
                                        err);

    g_slist_free_full(targets, (GDestroyNotify)lr_packagetarget_free);

    return ret;
}
