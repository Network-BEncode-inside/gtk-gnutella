/*
 * $Id$
 *
 * Copyright (c) 2004, Raphael Manfredi
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
 * @ingroup lib
 * @file
 *
 * File watcher.
 *
 * Periodically monitors file and invoke processing callback
 * should the file change.
 *
 * @author Raphael Manfredi
 * @date 2004
 */

#include "common.h"

#include "watcher.h"
#include "atoms.h"
#include "cq.h"
#include "glib-missing.h"
#include "halloc.h"
#include "path.h"
#include "walloc.h"
#include "override.h"		/* Must be the last header included */

#define MONITOR_PERIOD_MS	(30*1000)	/**< 30 seconds */

/**
 * A monitored file.
 */
struct monitored {
	const char *filename;	/**< Filename to monitor */
	time_t mtime;			/**< Last known modified time */
	watcher_cb_t cb;		/**< Callback to invoke on change */
	gpointer udata;			/**< User supplied data to hand-out to callback */
};

static GHashTable *monitored;	/**< filename -> struct monitored */

/**
 * Compute the modified time of the file on disk.
 */
static time_t
watcher_mtime(const char *filename)
{
	filestat_t buf;

	if (-1 == stat(filename, &buf))
		return 0;

	return buf.st_mtime;
}

/**
 * Check each registered file for change -- hash table iterator callback.
 */
static void
watcher_check_mtime(gpointer unused_key, gpointer value, gpointer unused_udata)
{
	struct monitored *m = value;
	time_t new_mtime;

	(void) unused_key;
	(void) unused_udata;

	new_mtime = watcher_mtime(m->filename);

	if (new_mtime > m->mtime) {
		m->mtime = new_mtime;
		(*m->cb)(m->filename, m->udata);
	}
}

/**
 * Callout queue periodic event to perform periodic monitoring of the
 * registered files.
 */
static gboolean
watcher_timer(gpointer unused_udata)
{
	(void) unused_udata;

	g_hash_table_foreach(monitored, watcher_check_mtime, NULL);

	return TRUE;		/* Keep calling */
}

/**
 * Register new file to be monitored.
 *
 * If the file was already monitored, cancel the previous monitoring action
 * and replace it with this one.
 *
 * @param filename the file to monitor (string duplicated)
 * @param cb the callback to invoke when the file changes
 * @param udata extra data to pass to the callback, along with filename
 */
void
watcher_register(const char *filename, watcher_cb_t cb, gpointer udata)
{
	struct monitored *m;

	WALLOC0(m);
	m->filename = atom_str_get(filename);
	m->cb = cb;
	m->udata = udata;
	m->mtime = watcher_mtime(filename);

	if (g_hash_table_lookup(monitored, filename) != NULL)
		watcher_unregister(filename);

	gm_hash_table_insert_const(monitored, m->filename, m);
}

/**
 * Same as watcher_register() but a path, i.e. a (dir, base) tuple is
 * given instead of a complete filename.
 */
void
watcher_register_path(const file_path_t *fp, watcher_cb_t cb, gpointer udata)
{
	char *path;

	path = make_pathname(fp->dir, fp->name);
	watcher_register(path, cb, udata);
	HFREE_NULL(path);
}

/**
 * Free monitoring structure.
 */
static void
watcher_free(struct monitored *m)
{
	atom_str_free(m->filename);
	WFREE(m);
}

/**
 * Cancel monitoring of specified file.
 */
void
watcher_unregister(const char *filename)
{
	struct monitored *m;

	m = g_hash_table_lookup(monitored, filename);

	g_assert(m != NULL);

	g_hash_table_remove(monitored, m->filename);
	watcher_free(m);
}

/**
 * Same as watcher_unregister() but a path, i.e. a (dir, base) tuple is
 * given instead of a complete filename.
 */
void
watcher_unregister_path(const file_path_t *fp)
{
	char *path;

	path = make_pathname(fp->dir, fp->name);
	watcher_unregister(path);
	HFREE_NULL(path);
}

/**
 * Initialization.
 */
void
watcher_init(void)
{
	monitored = g_hash_table_new(g_str_hash, g_str_equal);
	cq_periodic_main_add(MONITOR_PERIOD_MS, watcher_timer, NULL);
}

/**
 * Free monitored structure -- hash table iterator callback.
 */
static void
free_monitored_kv(gpointer unused_key, gpointer value, gpointer unused_udata)
{
	struct monitored *m = value;

	(void) unused_key;
	(void) unused_udata;
	watcher_free(m);
}

/**
 * Final cleanup.
 */
void
watcher_close(void)
{
	g_hash_table_foreach(monitored, free_monitored_kv, NULL);
	gm_hash_table_destroy_null(&monitored);
}

/* vi: set ts=4 sw=4 cindent: */
