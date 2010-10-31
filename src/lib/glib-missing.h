/*
 * $Id$
 *
 * Copyright (c) 2003, Raphael Manfredi
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
 * Missing functions in the Glib 1.2.
 *
 * Functions that should be in glib-1.2 but are not.
 * They are all prefixed with "gm_" as in "Glib Missing".
 *
 * We also include FIXED versions of glib-1.2 routines that are broken
 * and make sure those glib versions are never called directly.
 *
 * @author Raphael Manfredi
 * @date 2003
 */

#ifndef _glib_missing_h_
#define _glib_missing_h_

#include "common.h"

#ifdef USE_GLIB1
typedef gboolean (*GEqualFunc)(gconstpointer a, gconstpointer b);

typedef struct GMemVTable {
	gpointer	(*gmvt_malloc)		(gsize n_bytes);
	gpointer	(*gmvt_realloc)		(gpointer mem, gsize n_bytes);
	void		(*gmvt_free)		(gpointer mem);
	/* optional */
	gpointer	(*gmvt_calloc)		(gsize n_blocks, gsize n_block_bytes);
	gpointer	(*gmvt_try_malloc)	(gsize n_bytes);
	gpointer	(*gmvt_try_realloc)	(gpointer mem, gsize n_bytes);
} GMemVTable;
#endif

/*
 * Public interface.
 */

void gm_mem_set_safe_vtable(void);

gboolean gm_slist_is_looping(const GSList *slist);
GSList *gm_slist_insert_after(GSList *list, GSList *lnk, gpointer data);

GList *gm_list_insert_after(GList *list, GList *lnk, gpointer data);

#ifdef USE_GLIB1
GList *g_list_delete_link(GList *l, GList *lnk);
GSList *g_slist_delete_link(GSList *sl, GSList *lnk);
GList *g_list_insert_before(GList *l, GList *lk, gpointer data);
GString *g_string_append_len(GString *gs, const char *val, gssize len);

void g_hash_table_replace(GHashTable *ht, gpointer key, gpointer value);
gboolean gm_hash_table_remove(GHashTable *ht, gconstpointer key);

void g_mem_set_vtable(GMemVTable *vtable);
gboolean g_mem_is_system_malloc(void);

typedef int (*GCompareDataFunc)
	(gconstpointer a, gconstpointer b, gpointer user_data);

GList *g_list_sort_with_data(
	GList *l, GCompareDataFunc cmp, gpointer user_data);

#define g_string_printf		g_string_sprintf
#endif

#ifdef USE_GLIB2
#define gm_hash_table_remove	g_hash_table_remove
#endif

/**
 * Needs to be defined if we are not using Glib 2
 */
#ifndef USE_GLIB2

#ifndef HAS_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t dst_size);
#endif /* HAS_STRLCPY */

#ifndef HAS_STRLCAT
size_t strlcat(char *dst, const char *src, size_t dst_size);
#endif /* HAS_STRLCAT */

#define g_string_printf g_string_sprintf
#define g_strlcpy strlcpy
#define g_strlcat strlcat
#endif

char *gm_string_finalize(GString *gs);

size_t gm_vsnprintf(char *str, size_t n, char const *fmt, va_list args);
size_t gm_snprintf(char *str, size_t n,
	char const *fmt, ...) G_GNUC_PRINTF (3, 4);

void gm_savemain(int argc, char **argv, char **env);
const char *gm_getproctitle(void);
void gm_setproctitle(const char *title);

static inline void
gm_hash_table_insert_const(GHashTable *ht,
	gconstpointer key, gconstpointer value)
{
	g_hash_table_insert(ht, (gpointer) key, (gpointer) value);
}

static inline void
gm_hash_table_replace_const(GHashTable *ht,
	gconstpointer key, gconstpointer value)
{
	g_hash_table_replace(ht, (gpointer) key, (gpointer) value);
}

GSList *gm_hash_table_all_keys(GHashTable *ht);
void gm_hash_table_foreach_key(GHashTable *ht, GFunc func, gpointer user_data);

static inline GSList *
gm_slist_prepend_const(GSList *sl, gconstpointer value)
{
	return g_slist_prepend(sl, (gpointer) value);
}

/*
 * The G_*LIST_FOREACH_* macros are supposed to be used with ``func'' being
 * a function declared ``static inline'' whereas the protoype MUST match
 * ``GFunc''. ``func'' is not assigned to a variable so that the compiler
 * can prevent any function call overhead along with ``inline''.
 * These macros were rejected by the GLib maintainers so we can safely use
 * the G_ prefix.
 */

/* NB: Sub-statement func is evaluated more than once! */
#define G_LIST_FOREACH(list, func) \
	G_STMT_START { \
		GList *l_ = (list); \
		while (NULL != l_) { \
			func(l_->data); \
			l_ = g_list_next(l_); \
		} \
	} G_STMT_END

#define G_LIST_FOREACH_WITH_DATA(list, func, user_data) \
	G_STMT_START { \
		GList *l_ = (list); \
		gpointer user_data_ = (user_data); \
		while (NULL != l_) { \
			func(l_->data, user_data_); \
			l_ = g_list_next(l_); \
		} \
	} G_STMT_END

#define G_LIST_FOREACH_SWAPPED(list, func, user_data) \
	G_STMT_START { \
		GList *l_ = (list); \
		gpointer user_data_ = (user_data); \
		while (NULL != l_) { \
			func(user_data_, l_->data); \
			l_ = g_list_next(l_); \
		} \
	} G_STMT_END

/* NB: Sub-statement func is evaluated more than once! */
#define G_SLIST_FOREACH(slist, func) \
	G_STMT_START { \
		GSList *sl_ = (slist); \
		while (NULL != sl_) { \
			func(sl_->data); \
			sl_ = g_slist_next(sl_); \
		} \
	} G_STMT_END

/* NB: Sub-statement func is evaluated more than once! */
#define G_SLIST_FOREACH_WITH_DATA(slist, func, user_data) \
	G_STMT_START { \
		GSList *sl_ = (slist); \
		gpointer user_data_ = (user_data); \
		while (NULL != sl_) { \
			func(sl_->data, user_data_); \
			sl_ = g_slist_next(sl_); \
		} \
	} G_STMT_END


/* NB: Sub-statement func is evaluated more than once! */
#define G_SLIST_FOREACH_SWAPPED(slist, func, user_data) \
	G_STMT_START { \
		GSList *sl_ = (slist); \
		gpointer user_data_ = (user_data); \
		while (NULL != sl_) { \
			func(user_data_, sl_->data); \
			sl_ = g_slist_next(sl_); \
		} \
	} while(0)

#define GM_SLIST_FOREACH(slist, iter) \
	for ((iter) = (slist); NULL != (iter); (iter) = g_slist_next(iter))

/***
 *** Extra logging conveniences not defined by glib-1.x or glib-2.x.
 ***/

#if defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define g_info(...)		g_log (G_LOG_DOMAIN,		\
							   G_LOG_LEVEL_INFO,	\
							   __VA_ARGS__)
#if !GLIB_CHECK_VERSION(2,0,0)
#define g_debug(...)	g_log (G_LOG_DOMAIN,		\
							   G_LOG_LEVEL_DEBUG,	\
							   __VA_ARGS__)
#endif
#elif defined (__GNUC__)
#define g_info(format...)	g_log (G_LOG_DOMAIN,		\
								   G_LOG_LEVEL_INFO,	\
								   format)
#if !GLIB_CHECK_VERSION(2,0,0)
#define g_debug(format...)	g_log (G_LOG_DOMAIN,		\
								   G_LOG_LEVEL_DEBUG,	\
								   format)
#endif
#else	/* !__GNUC__ */
static inline void
g_info (const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  g_logv(G_LOG_DOMAIN, G_LOG_LEVEL_INFO, format, args);
  va_end(args);
}
#if !GLIB_CHECK_VERSION(2,0,0)
static void
g_debug (const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  g_logv(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, format, args);
  va_end(args);
}
#endif
#endif	/* !__GNUC__ */

/* vi: set ts=4 sw=4: */
#endif	/* _glib_missing_h_ */
