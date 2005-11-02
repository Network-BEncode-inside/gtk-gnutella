/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
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
 * Explicit-width block allocator, based on zalloc().
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#ifndef _walloc_h_
#define _walloc_h_

#include <glib.h>

/*
 * Under REMAP_ZALLOC control, those routines are remapped to malloc/free.
 * Under TRACK_ZALLOC, we keep tack of the allocation places.
 */

#if defined(USE_DMALLOC) && !defined(REMAP_ZALLOC)
#define REMAP_ZALLOC
#endif

#ifdef REMAP_ZALLOC

#ifdef TRACK_ZALLOC
#error "TRACK_ZALLOC and REMAP_ZALLOC are mutually exclusive"
#endif

#define walloc(s)			g_malloc(s)
#define walloc0(s)			g_malloc0(s)
#define wcopy(p,s)			g_memdup((p), (s))
#define wfree(p,s)			g_free(p)
#define wrealloc(p,o,n)		g_realloc((p), (n));

#else	/* !REMAP_ZALLOC */

gpointer walloc(size_t size);
gpointer walloc0(size_t size);
void wfree(gpointer ptr, size_t size);
gpointer wrealloc(gpointer old, size_t old_size, size_t new_size);

static inline WARN_UNUSED_RESULT gpointer
wcopy(gconstpointer ptr, size_t size)
{
	gpointer cp = walloc(size);
	memcpy(cp, ptr, size);
	return cp;
}

#endif	/* REMAP_ZALLOC */

#ifdef TRACK_ZALLOC

#define walloc(s)			walloc_track(s, __FILE__, __LINE__)
#define wcopy(p,s)			wcopy_track(p, s, __FILE__, __LINE__)
#define walloc0(s)			walloc0_track(s, __FILE__, __LINE__)
#define wrealloc(p,o,n)		wrealloc_track(p, o, n, __FILE__, __LINE__)

gpointer walloc_track(size_t size, gchar *file, gint line);
gpointer walloc0_track(size_t size, gchar *file, gint line);
gpointer wcopy_track(gconstpointer, size_t size, gchar *file, gint line);
gpointer wrealloc_track(gpointer old, size_t old_size, size_t new_size,
	gchar *file, gint line);

#endif	/* TRACK_ZALLOC */

void wdestroy(void);

#define WFREE_NULL(p,size)	\
G_STMT_START {				\
	if (p) {				\
		wfree(p,size);		\
		p = NULL;			\
	}						\
} G_STMT_END

#endif /* _walloc_h_ */
/* vi: set ts=4 sw=4 cindent: */
