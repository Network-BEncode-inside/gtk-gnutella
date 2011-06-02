/*
 * $Id$
 *
 * Copyright (c) 2001-2008, Christian Biere & Raphael Manfredi
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
 * ASCII functions.
 *
 * @author Raphael Manfredi
 * @date 2001-2008
 * @author Christian Biere
 * @date 2003-2008
 */

#ifndef _ascii_h_
#define _ascii_h_

#include "common.h"
#include "casts.h"

int ascii_strcasecmp(const char *s1, const char *s2);
int ascii_strncasecmp(const char *s1, const char *s2, size_t len);

/**
 * Converts a hexadecimal char (0-9, A-F, a-f) to an integer.
 *
 * @param c the character to convert.
 * @return 0..15 for valid hexadecimal ASCII characters, -1 otherwise.
 */
static inline int
hex2int_inline(guchar c)
{
	extern const gint8 *hex2int_tab;
	return hex2int_tab[c];
}

/**
 * Converts a decimal char (0-9) to an integer.
 *
 * @param c the character to convert.
 * @return 0..9 for valid decimal ASCII characters, -1 otherwise.
 */
static inline int
dec2int_inline(guchar c)
{
	extern const gint8 *dec2int_tab;
	return dec2int_tab[c];
}

/**
 * Converts an alphanumeric char (0-9, A-Z, a-z) to an integer.
 *
 * @param c the character to convert.
 * @return 0..9 for valid alphanumeric ASCII characters, -1 otherwise.
 */
static inline int
alnum2int_inline(guchar c)
{
	extern const gint8 *alnum2int_tab;
	return alnum2int_tab[c];
}

/**
 * ctype-like functions that allow only ASCII characters whereas the locale
 * would allow others. The parameter doesn't have to be casted to (unsigned
 * char) because these functions return false for everything out of [0..127].
 *
 * GLib 2.x has similar macros/functions but defines only a subset.
 */

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_blank(int c)
{
	return c == 32 || c == 9;	/* space, tab */
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_cntrl(int c)
{
	return (c >= 0 && c <= 31) || c == 127;
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_digit(int c)
{
	return c >= 48 && c <= 57;	/* 0-9 */
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_xdigit(int c)
{
	return -1 != hex2int_inline(c) && !(c & ~0x7f);
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_upper(int c)
{
	return c >= 65 && c <= 90;		/* A-Z */
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_lower(int c)
{
	return c >= 97 && c <= 122;		/* a-z */
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_alpha(int c)
{
	return is_ascii_upper(c) || is_ascii_lower(c);	/* A-Z, a-z */
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_alnum(int c)
{
	return -1 != alnum2int_inline(c) && !(c & ~0x7f); /* A-Z, a-z, 0-9 */
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_space(int c)
{
	return c == 32 || (c >= 9 && c <= 13);
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_graph(int c)
{
	return c >= 33 && c <= 126;
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_print(int c)
{
	return is_ascii_graph(c) || c == 32;
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT gboolean
is_ascii_punct(int c)
{
	return c >= 33 && c <= 126 && !is_ascii_alnum(c);
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT int
ascii_toupper(int c)
{
	return is_ascii_lower(c) ? c - 32 : c;
}

static inline G_GNUC_CONST WARN_UNUSED_RESULT int
ascii_tolower(int c)
{
	return is_ascii_upper(c) ? c + 32 : c;
}

/**
 * Skips over all ASCII space characters starting at ``s''.
 *
 * @return a pointer to the first non-space character starting from s.
 */
static inline WARN_UNUSED_RESULT char *
skip_ascii_spaces(const char *s)
{
	while (is_ascii_space(*s))
		s++;

	return deconstify_char(s);
}

/**
 * Skips over all characters which are not ASCII spaces starting at ``s''.
 *
 * @return a pointer to the first space or NUL character starting from s.
 */
static inline WARN_UNUSED_RESULT char *
skip_ascii_non_spaces(const char *s)
{
	while ('\0' != *s && !is_ascii_space(*s))
		s++;

	return deconstify_char(s);
}

/**
 * Skips over all characters which are ASCII alphanumerical characters
 * starting at ``s''.
 *
 * @return a pointer to the first non-alphanumerical or NUL character
 * starting from s.
 */
static inline WARN_UNUSED_RESULT char *
skip_ascii_alnum(const char *s)
{
	while (is_ascii_alnum(*s))
		s++;

	return deconstify_char(s);
}

/**
 * Skips over all ASCII blank characters starting at ``s''.
 *
 * @return A pointer to the first non-blank character starting from s.
 */
static inline WARN_UNUSED_RESULT char *
skip_ascii_blanks(const char *s)
{
	while (is_ascii_blank(*s))
		s++;

	return deconstify_char(s);
}

void ascii_strlower(char *dst, const char *src);
int ascii_strcasecmp_delimit(const char *a, const char *b,
		const char *delimit);
char *ascii_strcasestr(const char *haystack, const char *needle);
int ascii_strcmp_delimit(const char *a, const char *b, const char *delimit);
size_t ascii_chomp_trailing_spaces(char *str, size_t len);

guint ascii_strcase_hash(gconstpointer key);
int ascii_strcase_eq(gconstpointer a, gconstpointer b);

#endif /* _ascii_h_ */

/* vi: set ts=4 sw=4 cindent: */
