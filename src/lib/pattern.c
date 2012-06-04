/*
 * Copyright (c) 2001-2004, Raphael Manfredi
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
 * Pattern matching.
 *
 * @author Raphael Manfredi
 * @date 2001-2004
 */

#include "common.h"

#include "misc.h"
#include "pattern.h"
#include "halloc.h"
#include "walloc.h"
#include "override.h"		/* Must be the last header included */

/**
 * Initialize pattern data structures.
 */
void
pattern_init(void)
{
	/* Nothing to do */
}

/**
 * Cleanup data structures.
 */
void
pattern_close(void)
{
	/* Nothing to do */
}

/*
 * Pattern matching (substrings, not regular expressions)
 *
 * The algorithm used below is the one described in Communications
 * of the ACM, volume 33, number 8, August 1990, by Daniel M. Sunday
 * It's a variant of the classical Boyer-Moore search, but with a small
 * enhancement that can make a difference.
 */

/**
 * Compile given string pattern by computing the delta shift table.
 * The pattern string given is duplicated.
 *
 * @return a compiled pattern structure.
 */
cpattern_t *
pattern_compile(const char *pattern)
{
	cpattern_t *p;
	size_t plen, i, *pd;
	const uchar *c;

	WALLOC(p);
	p->pattern = h_strdup(pattern);
	p->len = plen = strlen(p->pattern);
	p->duped = TRUE;

	plen++;			/* Avoid increasing within the loop */
	pd = p->delta;

	for (i = 0; i < ALPHA_SIZE; i++)
		*pd++ = plen;

	plen--;			/* Restore original pattern length */

 	c = cast_to_constpointer(pattern);
	for (pd = p->delta, i = 0; i < plen; c++, i++)
		pd[*c] = plen - i;

	return p;
}

/**
 * Same as pattern_compile(), but the pattern string is NOT duplicated,
 * and its length is known upon entry.
 *
 * @attention
 * NB: There is no pattern_free_fast(), just call pattern_free() on the result.
 */
G_GNUC_HOT cpattern_t *
pattern_compile_fast(const char *pattern, size_t plen)
{
	cpattern_t *p;
	size_t i, *pd;
	const uchar *c;

	WALLOC(p);
	p->pattern = pattern;
	p->len = plen;
	p->duped = FALSE;

	plen++;			/* Avoid increasing within the memset() inlined macro */
	pd = p->delta;

	for (i = 0; i < ALPHA_SIZE; i++)
		*pd++ = plen;

	plen--;			/* Restore original pattern length */

 	c = cast_to_constpointer(pattern);
	for (pd = p->delta, i = 0; i < plen; c++, i++)
		pd[*c] = plen - i;

	return p;
}

/**
 * Dispose of compiled pattern.
 */
void
pattern_free(cpattern_t *cpat)
{
	if (cpat->duped) {
		hfree(deconstify_gchar(cpat->pattern));
		cpat->pattern = NULL; /* Don't use HFREE_NULL b/c of lvalue cast */
	}
	WFREE(cpat);
}

/**
 * Dispose of compiled pattern and nullify its pointer.
 */
void
pattern_free_null(cpattern_t **cpat_ptr)
{
	cpattern_t *cpat = *cpat_ptr;

	if (cpat != NULL) {
		pattern_free(cpat);
		*cpat_ptr = NULL;
	}
}

/**
 * Quick substring search algorithm.  It looks for the compiled pattern
 * with `text', from left to right.  The `tlen' argument is the length
 * of the text, and can left to 0, in which case it will be computed.
 *
 * @return pointer to beginning of matching substring, NULL if not found.
 */
G_GNUC_HOT const char *
pattern_qsearch(
	const cpattern_t *cpat,	/**< Compiled pattern */
	const char *text,		/**< Text we're scanning */
	size_t tlen,			/**< Text length, 0 = compute strlen(text) */
	size_t toffset,			/**< Offset within text for search start */
	qsearch_mode_t word)	/**< Beginning/whole word matching? */
{
	const char *p;			/* Pointer within string pattern */
	const char *t;			/* Pointer within text */
	const char *tp;			/* Initial local search text pointer */
	const char *start;		/* Start of matching */
	const char *end;		/* End of text (first byte after physical end) */
	size_t i;				/* Position within pattern string */
	size_t plen;

	if (!tlen)
		tlen = strlen(text);
	start = text + toffset;
	end = text + tlen;
	tp = start;
	plen = cpat->len;

	while (tp + plen <= end) {		/* Enough text left for matching */

		for (p = cpat->pattern, t = tp, i = 0; i < plen; p++, t++, i++)
			if (*p != *t)
				break;				/* Mismatch, stop looking here */

		if (i == plen) {			/* OK, we got a pattern match */
			bool at_begin = FALSE;

			if (word == qs_any)
				return tp;			/* Start of substring */

			/*
			 * They set `word', so we must look whether we are at the start
			 * of a word, i.e. if it is either the beginning of the text,
			 * or if the character before is a non-alphanumeric character.
			 */

			g_assert(word == qs_begin || word == qs_whole);

			if (tp == text) {					/* At beginning of text */
				if (word == qs_begin) return tp;
				else at_begin = TRUE;
			} else if (0x20 == *(tp-1)) {	/* At word boundary */
				if (word == qs_begin) return tp;
				else at_begin = TRUE;
			}

			if (at_begin && word == qs_whole) {
				if (&tp[plen] == end)			/* At end of string */
					return tp;
				else if (0x20 == tp[plen])
					return tp; /* At word boundary after */
			}

			/* Fall through */
		}

		tp += cpat->delta[(uchar) tp[plen]]; /* Continue search there */
	}

	return NULL;		/* Not found */
}

/* vi: set ts=4 sw=4 cindent: */
