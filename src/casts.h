/*
 * $Id$
 *
 * Copyright (c) 2006 Christian Biere
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
 * @file
 *
 * Functions for safer casting. 
 *
 * @author Christian Biere
 * @date 2006
 */

#ifndef _casts_h_
#define _casts_h_

/* @note This file is only for inclusion by common.h. */

/**
 * Cast a ``const gchar *'' to ``gchar *''. This allows the compiler to
 * print a diagnostic message if you accidently try to deconstify an
 * incompatible type. A direct typecast would hide such a mistake.
 */
static inline G_GNUC_CONST WARN_UNUSED_RESULT gchar *
deconstify_gchar(const gchar *p)
{
	return (gchar *) p;
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT guint32 *
deconstify_guint32(const guint32 *p)
{
	return (guint32 *) p;
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gpointer
deconstify_gpointer(gconstpointer p)
{
	return (gpointer) p;
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gconstpointer
cast_to_gconstpointer(gconstpointer p)
{
	return p; 
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gpointer
cast_to_gpointer(gpointer p)
{
	return p;
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gchar *
cast_to_gchar_ptr(gpointer p)
{
	return p;
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT guchar *
cast_to_guchar_ptr(gpointer p)
{
	return p;
}

/**
 * FIXME: Use uintptr_t if available.
 */
static inline G_GNUC_CONST WARN_UNUSED_RESULT gulong
cast_ptr_to_uintptr(gpointer p)
{
	gulong u = (gulong) p;
	STATIC_ASSERT(sizeof u >= sizeof p);
	return u;
}

/**
 * FIXME: Use uintptr_t if available.
 */
static inline G_GNUC_CONST WARN_UNUSED_RESULT gpointer
cast_uintptr_to_ptr(gulong u)
{
	gpointer p = (gpointer) u;
	STATIC_ASSERT(sizeof p >= sizeof u);
	return p;
}

typedef void (*func_ptr_t)(void);

static inline G_GNUC_CONST WARN_UNUSED_RESULT gpointer
cast_func_to_gpointer(func_ptr_t f)
{
	union {
		func_ptr_t f;
		gpointer p;
	} u;

	u.f = f;
	return u.p;
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT func_ptr_t
cast_gpointer_to_func(gconstpointer p)
{
	union {
		func_ptr_t f;
		gconstpointer p;
	} u;

	u.p = p;
	return u.f;
}

static inline size_t
ptr_diff(gconstpointer a, gconstpointer b)
{
	return (gchar *) a - (gchar *) b;
}
	
#endif /* _casts_h_ */

/* vi: set ts=4 sw=4 cindent: */
