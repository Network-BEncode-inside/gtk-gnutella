/*
 * $Id$
 *
 * Copyright (c) 2002, Vidar Madsen
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup core
 * @file
 *
 * Structure for storing meta-information about files being downloaded.
 *
 * @author Vidar Madsen
 * @date 2002
 */

#ifndef _core_fileinfo_h_
#define _core_fileinfo_h_

#include "share.h"						/* For shared_file_t */
#include "if/core/fileinfo.h"

/*
 * Shared core constants
 */

#define FI_LOW_SRC_COUNT	5			/**< Few sources known if beneath */

/*
 * Operating flags.
 */

#define FI_F_SUSPEND		0x00000001U	/**< Marked "suspended" new downloads */
#define FI_F_DISCARD		0x00000002U	/**< Discard fileinfo if refcount = 0 */
#define FI_F_TRANSIENT		0x40000000U	/**< Don't persist to disk */
#define FI_F_MARK			0x80000000U	/**< Marked during traversal */

/*
 * Public interface.
 */

void file_info_init(void);
void file_info_init_post(void);
void file_info_scandir(const gchar *dir);
gboolean file_info_has_trailer(const gchar *path);
void file_info_retrieve(void);
void file_info_store(void);
void file_info_store_binary(fileinfo_t *fi);
void file_info_store_if_dirty(void);
void file_info_set_discard(fileinfo_t *fi, gboolean state);
enum dl_chunk_status file_info_find_hole(
	struct download *d, filesize_t *from, filesize_t *to);
gboolean file_info_find_available_hole(struct download *d,
	GSList *ranges, filesize_t *from, filesize_t *to);
void file_info_merge_adjacent(fileinfo_t *fi);
void file_info_clear_download(struct download *d, gboolean lifecount);
enum dl_chunk_status file_info_chunk_status(
	fileinfo_t *fi, filesize_t from, filesize_t to);
void file_info_reset(fileinfo_t *fi);
void file_info_recreate(struct download *d);
fileinfo_t *file_info_get(
	gchar *file, const gchar *path, filesize_t size, gchar *sha1,
	gboolean file_size_known);
void file_info_strip_binary(fileinfo_t *fi);
void file_info_strip_binary_from_file(fileinfo_t *fi, const gchar *file);
gboolean file_info_got_sha1(fileinfo_t *fi, const gchar *sha1);
void file_info_size_known(struct download *d, filesize_t size);
void file_info_update(struct download *d, filesize_t from, filesize_t to,
	enum dl_chunk_status status);
enum dl_chunk_status file_info_pos_status(fileinfo_t *fi,
	filesize_t pos /*, filesize_t *start, filesize_t *end */);
void file_info_close(void);
void file_info_close_pre(void);
void file_info_try_to_swarm_with(
	gchar *file_name, guint32 idx, const host_addr_t addr,
	guint16 port, gchar *sha1);
void file_info_spot_completed_orphans(void);
void file_info_add_source(fileinfo_t *fi, struct download *dl);
void file_info_add_new_source(fileinfo_t *fi, struct download *dl);
void file_info_remove_source(
    fileinfo_t *fi, struct download *dl, gboolean discard);
void file_info_timer(void);
void file_info_unlink(fileinfo_t *fi);
void file_info_upload_stop(fileinfo_t *fi, const gchar *reason);
void file_info_changed(fileinfo_t *fi);

shared_file_t *file_info_shared_sha1(const gchar *sha1);
gint file_info_available_ranges(fileinfo_t *fi, gchar *buf, gint size);
gboolean file_info_restrict_range(
	fileinfo_t *fi, filesize_t start, filesize_t *end);

fileinfo_t *file_info_has_identical(
	gchar *file, filesize_t size, gchar *sha1);

fileinfo_t *file_info_get_browse(const gchar *name);
void file_info_remove(fileinfo_t *fi);

/***
 *** Inlined routines.
 ***/

/**
 * Return amount of currently active sources.
 */
static inline G_GNUC_CONST WARN_UNUSED_RESULT guint32
fi_alive_count(fileinfo_t *fi)
{
	return fi->aqueued_count + fi->pqueued_count + fi->recvcount;
}

#endif /* _core_fileinfo_h_ */

/* vi: set ts=4 sw=4 cindent: */
