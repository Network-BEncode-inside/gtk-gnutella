/*
 * $Id$
 *
 * Copyright (c) 2004, Christian Biere
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
 * Asynchronous DNS lookup.
 *
 * @author Christian Biere
 * @date 2004
 */

#include "common.h"

RCSID("$Id$")

#include "adns.h"
#include "atoms.h"
#include "inputevt.h"
#include "misc.h"
#include "glib-missing.h"
#include "tm.h"
#include "walloc.h"
#include "socket.h"

#include "override.h"		/* Must be the last header included */

static guint32 common_dbg = 0;	/**< @bug XXX need to init lib's props --RAM */

/* private data types */

struct adns_common {
	void (*user_callback)(void);
	gpointer user_data;
	gboolean reverse;
};

struct adns_reverse_query {
	host_addr_t addr;
};

struct adns_query {
	enum net_type net;
	gchar hostname[MAX_HOSTLEN + 1];
};

struct adns_reply {
	gchar hostname[MAX_HOSTLEN + 1];
	host_addr_t addrs[10];
};

struct adns_reverse_reply {
	gchar hostname[MAX_HOSTLEN + 1];
	host_addr_t addr;
};

struct adns_request {
	struct adns_common common;
	union {
		struct adns_query by_addr;
		struct adns_reverse_query reverse;
	} query;
};

struct adns_response {
	struct adns_common common;
	union {
		struct adns_reply by_addr;
		struct adns_reverse_reply reverse;
	} reply;
};

typedef struct adns_async_write {
	struct adns_request req;	/**< The original ADNS request */
	gchar *buf;					/**< Remaining data to write; walloc()ed */
	size_t pos;					/**< Read position */
	size_t size;				/**< Size of the buffer */
} adns_async_write_t;

typedef struct adns_cache_entry {
	const gchar *hostname;		/**< atom */
	time_t timestamp;
	size_t n;				/**< Number of addr items */
	guint id;
	host_addr_t addrs[1 /* pseudo-size */];
} adns_cache_entry_t;

static inline size_t
adns_cache_entry_size(size_t n)
{
	struct adns_cache_entry *entry;
	g_assert(n > 0);
	g_assert(((size_t) -1 - sizeof *entry) / sizeof entry->addrs[0] > n);

	return sizeof *entry + n * sizeof entry->addrs[0];
}

static inline size_t
count_addrs(const host_addr_t *addrs, size_t m)
{
	size_t n;

	for (n = 0; n < m; n++) {
		if (!is_host_addr(addrs[n]))
			break;
	}
	return n;
}
	
/**
 * Cache entries will expire after ADNS_CACHE_TIMEOUT seconds.
 */
#define ADNS_CACHE_TIMEOUT (60)
/**
 * Cache max. ADNS_CACHE_SIZE of adns_cache_entry_t entries.
 */
#define ADNS_CACHE_MAX_SIZE (1024)

static const gchar adns_process_title[] = "DNS helper for gtk-gnutella";

typedef struct adns_cache_struct {
	GHashTable *ht;
	guint pos;
	gint timeout;
	adns_cache_entry_t *entries[ADNS_CACHE_MAX_SIZE];
} adns_cache_t;

static adns_cache_t *adns_cache = NULL;

/* private variables */

static gint adns_query_fd = -1;
static guint adns_query_event_id = 0;
static guint adns_reply_event_id = 0;
static gboolean is_helper = FALSE;		/**< Are we the DNS helper process? */

/**
 * Private macros.
 */

#define CLOSE_IF_VALID(fd)	\
do {						\
	if (-1 != (fd))	{		\
		close(fd);			\
		fd = -1;			\
	} 						\
} while(0)

/**
 * Private functions.
 */

static adns_cache_t *
adns_cache_init(void)
{
	adns_cache_t *cache;
	size_t i;

	cache = g_malloc(sizeof *cache);
	cache->timeout = ADNS_CACHE_TIMEOUT;
	cache->ht = g_hash_table_new(g_str_hash, g_str_equal);
	cache->pos = 0;
	for (i = 0; i < G_N_ELEMENTS(cache->entries); i++) {
		cache->entries[i] = NULL;
	}
	return cache;
}

/* these are not needed anywhere else so undefine them */
#undef ADNS_CACHE_MAX_SIZE
#undef ADNS_CACHE_TIMEOUT

static inline adns_cache_entry_t *
adns_cache_get_entry(adns_cache_t *cache, guint i)
{
	adns_cache_entry_t *entry;
	
	g_assert(cache);
	g_assert(i < G_N_ELEMENTS(cache->entries));

	entry = cache->entries[i];
	if (entry) {
		g_assert(i == entry->id);
		g_assert(entry->hostname);
		g_assert(entry->n > 0);
	}
	return entry;
}

static void
adns_cache_free_entry(adns_cache_t *cache, guint i)
{
	adns_cache_entry_t *entry;

	g_assert(cache);
	g_assert(i < G_N_ELEMENTS(cache->entries));

	entry = cache->entries[i];
	if (entry) {
		g_assert(i == entry->id);
		g_assert(entry->hostname);
		g_assert(entry->n > 0);

		atom_str_free_null(&entry->hostname);
		wfree(entry, adns_cache_entry_size(entry->n));
		cache->entries[i] = NULL;
	}
}

/**
 * Frees all memory allocated by the cache and returns NULL.
 */
void
adns_cache_free(adns_cache_t *cache)
{
	guint i;

	g_assert(cache);
	g_assert(cache->ht);

	for (i = 0; i < G_N_ELEMENTS(cache->entries); i++) {
		adns_cache_free_entry(cache, i);
	}
	g_hash_table_destroy(cache->ht);
	cache->ht = NULL;
	G_FREE_NULL(cache);
}

/**
 * Adds ``hostname'' and ``addr'' to the cache. The cache is implemented
 * as a wrap-around FIFO. In case it's full, the oldest entry will be
 * overwritten.
 */
static void
adns_cache_add(adns_cache_t *cache, time_t now,
	const gchar *hostname, const host_addr_t *addrs, size_t n)
{
	adns_cache_entry_t *entry;
	size_t i;
	
	g_assert(NULL != addrs);
	g_assert(NULL != cache);
	g_assert(NULL != hostname);
	g_assert(n > 0);

	g_assert(NULL == g_hash_table_lookup(cache->ht, hostname));

	g_assert(cache->pos < G_N_ELEMENTS(cache->entries));
	
	entry = adns_cache_get_entry(cache, cache->pos);
	if (entry) {
		g_assert(entry->hostname);
		g_assert(g_hash_table_lookup(cache->ht, entry->hostname) == entry);

		g_hash_table_remove(cache->ht, entry->hostname);
		adns_cache_free_entry(cache, cache->pos);
		entry = NULL;
	}

	entry = walloc(adns_cache_entry_size(n));
	entry->n = n;
	entry->hostname = atom_str_get(hostname);
	entry->timestamp = now;
	entry->id = cache->pos;
	for (i = 0; i < entry->n; i++) {
		entry->addrs[i] = addrs[i];
	}
	g_hash_table_insert(cache->ht, deconstify_gchar(entry->hostname), entry);
	cache->entries[cache->pos++] = entry;
	cache->pos %= G_N_ELEMENTS(cache->entries);
}

/**
 * Looks for ``hostname'' in ``cache'' wrt to cache->timeout. If
 * ``hostname'' is not found or the entry is expired, FALSE will be
 * returned. Expired entries will be removed! ``addr'' is allowed to
 * be NULL, otherwise the cached IP will be stored into the variable
 * ``addr'' points to.
 *
 * @param addrs An array of host_addr_t items. If not NULL, up to
 *              ``n'' items will be copied from the cache.
 * @param n The number of items "addrs" can hold.
 * @return The number of cached addresses for the given hostname.
 */
static size_t
adns_cache_lookup(adns_cache_t *cache, time_t now,
	const gchar *hostname, host_addr_t *addrs, size_t n)
{
	adns_cache_entry_t *entry;

	g_assert(NULL != cache);
	g_assert(NULL != hostname);
	g_assert(0 == n || NULL != addrs);

	entry = g_hash_table_lookup(cache->ht, hostname);
	if (entry) {
		if (delta_time(now, entry->timestamp) < cache->timeout) {
			size_t i;

			for (i = 0; i < n; i++) {
				if (i < entry->n) {
					addrs[i] = entry->addrs[i];
					if (common_dbg > 0)
						g_message("adns_cache_lookup: \"%s\" cached (addr=%s)",
							entry->hostname, host_addr_to_string(addrs[i]));
				} else {
					addrs[i] = zero_host_addr;
				}
			}
		} else {
			if (common_dbg > 0)
				g_message("adns_cache_lookup: removing \"%s\" from cache",
						entry->hostname);

			g_hash_table_remove(cache->ht, hostname);
			adns_cache_free_entry(cache, entry->id);
			entry = NULL;
		}
	}

	return entry ? entry->n : 0;
}

/**
 * Transfers the data in `buf' of size `len' through `fd'. If `do_write' is
 * FALSE the buffer will be filled from `fd'. Otherwise, the data from the
 * buffer will be written to `fd'. The function returns only if all data
 * has been transferred or if an unrecoverable error occurs. This function
 * should only be used with a blocking `fd'.
 */
static gboolean
adns_do_transfer(gint fd, gpointer buf, size_t len, gboolean do_write)
{
	ssize_t ret;
	size_t n = len;

	while (n > 0) {
		if (common_dbg > 2)
			g_message("adns_do_transfer (%s): n=%lu",
			    do_write ? "write" : "read", (gulong) n);

		if (do_write)
			ret = write(fd, buf, n);
		else
			ret = read(fd, buf, n);

		if ((ssize_t) -1 == ret && !is_temporary_error(errno)) {
            /* Ignore the failure, if the parent process is gone.
               This prevents an unnecessary warning when quitting. */
            if (!is_helper || getppid() != 1)
			    g_warning("adns_do_transfer (%s): %s (errno=%d)",
				    do_write ? "write" : "read", g_strerror(errno), errno);
			return FALSE;
		} else if (0 == ret) {
			/*
			 * Don't warn on EOF if this is the child process and the
			 * parent is gone.
			 */
			if (!do_write && !(is_helper && getppid() == 1))
				g_warning("adns_do_transfer (%s): EOF",
					do_write ? "write" : "read");
			return FALSE;
		} else if (ret > 0) {
			n -= ret;
			buf = (gchar *) buf + ret;
		}
	}

	return TRUE;
}

/**
 * Read the complete buffer ``buf'' of size ``len'' from file descriptor ``fd''
 *
 * @return TRUE on success, FALSE if the operation failed
 */
static gboolean
adns_do_read(gint fd, gpointer buf, size_t len)
{
	return adns_do_transfer(fd, buf, len, FALSE);
}

/**
 * Write the complete buffer ``buf'' of size ``len'' to file descriptor ``fd''
 *
 * @return TRUE on success, FALSE if the operation failed
 */
static gboolean
adns_do_write(gint fd, gpointer buf, size_t len)
{
	return adns_do_transfer(fd, buf, len, TRUE);
}

/**
 * Copies user_callback and user_data from the query buffer to the
 * reply buffer. This function won't fail. However, if gethostbyname()
 * fails ``reply->addr'' will be set to zero.
 */
static void
adns_gethostbyname(const struct adns_request *req, struct adns_response *ans)
{
	g_assert(NULL != req);
	g_assert(NULL != ans);

	ans->common = req->common;

	if (req->common.reverse) {
		const struct adns_reverse_query *query = &req->query.reverse;
		struct adns_reverse_reply *reply = &ans->reply.reverse;
		const gchar *host;


		if (common_dbg > 1) {
			g_message("adns_gethostbyname: Resolving \"%s\" ...",
					host_addr_to_string(query->addr));
		}

		reply->addr = query->addr;
		host = host_addr_to_name(query->addr);
		g_strlcpy(reply->hostname, host ? host : "", sizeof reply->hostname);
	} else {
		const struct adns_query *query = &req->query.by_addr;
		struct adns_reply *reply = &ans->reply.by_addr;
		GSList *sl_addr, *sl;
		size_t i = 0;

		if (common_dbg > 1) {
			g_message("adns_gethostbyname: Resolving \"%s\" ...",
				query->hostname);
		}
		g_strlcpy(reply->hostname, query->hostname, sizeof reply->hostname);

		sl_addr = name_to_host_addr(query->hostname, query->net);
		for (sl = sl_addr; NULL != sl; sl = g_slist_next(sl)) {
			host_addr_t *addr = sl->data;
			g_assert(addr);
			if (i >= G_N_ELEMENTS(reply->addrs)) {
				break;
			}
			reply->addrs[i++] = *addr;
		}
		host_addr_free_list(&sl_addr);

		if (i < G_N_ELEMENTS(reply->addrs)) {
			reply->addrs[i] = zero_host_addr;
		}
	}
}

/**
 * The ``main'' function of the adns helper process (server).
 *
 * Simply reads requests (queries) from fd_in, performs a DNS lookup for it
 * and writes the result to fd_out. All operations should be blocking. Exits
 * in case of non-recoverable error during read or write.
 */
static void
adns_helper(gint fd_in, gint fd_out)
{
	g_set_prgname(adns_process_title);
	gm_setproctitle(g_get_prgname());

#ifdef SIGQUIT 
	set_signal(SIGQUIT, SIG_IGN);	/* Avoid core dumps on SIGQUIT */
#endif

	is_helper = TRUE;

	for (;;) {
		struct adns_request req;
		struct adns_response ans;
		size_t size;
		gpointer buf;

		if (!adns_do_read(fd_in, &req.common, sizeof req.common))
			break;
	
		if (req.common.reverse) {	
			size = sizeof req.query.reverse;
			buf = &req.query.reverse;
		} else {
			size = sizeof req.query.by_addr;
			buf = &req.query.by_addr;
		}

		if (!adns_do_read(fd_in, buf, size))
			break;

		adns_gethostbyname(&req, &ans);

		if (!adns_do_write(fd_out, &ans.common, sizeof ans.common))
			break;

		if (ans.common.reverse) {	
			size = sizeof ans.reply.reverse;
			buf = &ans.reply.reverse;
		} else {
			size = sizeof ans.reply.by_addr;
			buf = &ans.reply.by_addr;
		}

		if (!adns_do_write(fd_out, buf, size))
			break;
	}

	close(fd_in);
	close(fd_out);
	_exit(EXIT_SUCCESS);
}

static inline void
adns_invoke_user_callback(const struct adns_response *ans)
{
	if (ans->common.reverse) {
		const struct adns_reverse_reply *reply = &ans->reply.reverse;
		adns_reverse_callback_t func;

		func = (adns_reverse_callback_t) ans->common.user_callback;
		func(reply->hostname[0] != '\0' ? reply->hostname : NULL,
			ans->common.user_data);
	} else {
		const struct adns_reply *reply = &ans->reply.by_addr;
		adns_callback_t func;
		size_t n;
	
		n = count_addrs(reply->addrs, G_N_ELEMENTS(reply->addrs));
		func = (adns_callback_t) ans->common.user_callback;
		func(reply->addrs, n, ans->common.user_data);
	}
}

/**
 * Handles the query in synchronous (blocking) mode and is used if the
 * dns helper is busy i.e., the pipe buffer is full or in case the dns
 * helper is dead.
 */
static void
adns_fallback(const struct adns_request *req)
{
	struct adns_response ans;

	g_assert(req);
	adns_gethostbyname(req, &ans);
	g_assert(ans.common.user_callback);
	adns_invoke_user_callback(&ans);
}

static void
adns_reply_ready(const struct adns_response *ans)
{
	time_t now = tm_time();

	g_assert(ans);

	if (ans->common.reverse) {
		if (common_dbg > 1) {
			const struct adns_reverse_reply *reply = &ans->reply.reverse;
			
			g_message("adns_reply_ready: Resolved \"%s\" to \"%s\".",
				host_addr_to_string(reply->addr), reply->hostname);
		}
	} else {
		const struct adns_reply *reply = &ans->reply.by_addr;
		size_t num;

		num = count_addrs(reply->addrs, G_N_ELEMENTS(reply->addrs));
		num = MAX(1, num); /* For negative caching */
		
		if (common_dbg > 1) {
			size_t i;
			
			for (i = 0; i < num; i++) {
				g_message("adns_reply_ready: Resolved \"%s\" to \"%s\".",
					reply->hostname, host_addr_to_string(reply->addrs[i]));
			}
		}

		
		if (!adns_cache_lookup(adns_cache, now, reply->hostname, NULL, 0)) {
			adns_cache_add(adns_cache, now, reply->hostname, reply->addrs, num);
		}
	}


	g_assert(ans->common.user_callback);
	adns_invoke_user_callback(ans);
}

/**
 * Callback function for inputevt_add(). This function invokes the callback
 * function given in DNS query on the client-side i.e., gtk-gnutella itself.
 * It handles partial reads if necessary. In case of an unrecoverable error
 * the reply pipe will be closed and the callback will be lost.
 */
static void
adns_reply_callback(gpointer data, gint source, inputevt_cond_t condition)
{
	static struct adns_response ans;
	static gpointer buf;
	static size_t size, pos;

	g_assert(NULL == data);
	g_assert(condition & INPUT_EVENT_RX);

	for (;;) {
		ssize_t ret;
		size_t n;

		if (pos == size) {

			pos = 0;
			if (cast_to_gpointer(&ans.common) == buf) {
				if (ans.common.reverse) {
					buf = &ans.reply.reverse;
					size = sizeof ans.reply.reverse;
				} else {
					buf = &ans.reply.by_addr;
					size = sizeof ans.reply.by_addr;
				}
			} else {
				if (buf) {
					adns_reply_ready(&ans);
				}
				buf = &ans.common;
				size = sizeof ans.common;
			}
		}

		g_assert(buf);
		g_assert(size > 0);
		g_assert(pos < size);

		n = size - pos;
		ret = read(source, cast_to_gchar_ptr(buf) + pos, n);
		if ((ssize_t) -1 == ret) {
			if (!is_temporary_error(errno)) {
				g_warning("adns_reply_callback: read() failed: %s",
						g_strerror(errno));
				inputevt_remove(adns_reply_event_id);
				adns_reply_event_id = 0;
				g_warning("adns_reply_callback: removed myself");
				close(source);
			}
			break;
		} else if (0 != ret) {
			g_assert(ret > 0);
			g_assert((size_t) ret <= n);
			pos += (size_t) ret;
		}
	}
	return;
}

/**
 * Allocate a "spill" buffer of size `size'.
 */
static adns_async_write_t *
adns_async_write_alloc(const struct adns_request *req,
	gconstpointer buf, size_t size)
{
	adns_async_write_t *remain;

	g_assert(req);
	g_assert(buf);
	g_assert(size > 0);
	
	remain = walloc(sizeof *remain);
	remain->req = *req;
	remain->size = size;
	remain->buf = wcopy(buf, remain->size);
	remain->pos = 0;

	return remain;
}

/**
 * Dispose of the "spill" buffer.
 */
static void
adns_async_write_free(adns_async_write_t *remain)
{
	g_assert(remain);
	g_assert(remain->buf);
	g_assert(remain->size > 0);
	
	wfree(remain->buf, remain->size);
	wfree(remain, sizeof *remain);
}

/**
 * Callback function for inputevt_add(). This function pipes the query to
 * the server using the pipe in non-blocking mode, partial writes are handled
 * appropriately. In case of an unrecoverable error the query pipe will be
 * closed and the blocking adns_fallback() will be invoked.
 */
static void
adns_query_callback(gpointer data, gint dest, inputevt_cond_t condition)
{
	adns_async_write_t *remain = data;

	g_assert(NULL != remain);
	g_assert(NULL != remain->buf);
	g_assert(remain->pos < remain->size);
	g_assert(dest == adns_query_fd);
	g_assert(0 != adns_query_event_id);

	if (condition & INPUT_EVENT_EXCEPTION) {
		g_warning("adns_query_callback: write exception");
		goto abort;
	}

	while (remain->pos < remain->size) {
		ssize_t ret;
		size_t n;

		n = remain->size - remain->pos;
		ret = write(dest, &remain->buf[remain->pos], n);

		if (0 == ret) {
			errno = ECONNRESET;
			ret = (ssize_t) -1;
		}
		/* FALL THROUGH */
		if ((ssize_t) -1 == ret) {
			if (!is_temporary_error(errno))
				goto error;
			return;
		}

		g_assert(ret > 0);
		g_assert((size_t) ret <= n);
		remain->pos += (size_t) ret;
	}
	g_assert(remain->pos == remain->size);

	inputevt_remove(adns_query_event_id);
	adns_query_event_id = 0;

	goto done;	


error:
	g_warning("adns_query_callback: write() failed: %s", g_strerror(errno));
abort:
	g_warning("adns_query_callback: removed myself");
	inputevt_remove(adns_query_event_id);
	adns_query_event_id = 0;
	CLOSE_IF_VALID(adns_query_fd);
	g_warning("adns_query_callback: using fallback");
	adns_fallback(&remain->req);
done:
	adns_async_write_free(remain);
	return;
}

/* public functions */

/**
 * Initializes the adns helper i.e., fork()s a child process which will
 * be used to resolve hostnames asynchronously.
 */
void
adns_init(void)
{
	gint fd_query[2] = {-1, -1};
	gint fd_reply[2] = {-1, -1};
	pid_t pid;

	if (-1 == pipe(fd_query) || -1 == pipe(fd_reply)) {
		g_warning("adns_init: pipe() failed: %s", g_strerror(errno));
		goto prefork_failure;
	}
	
#ifdef SIGCHLD 
	set_signal(SIGCHLD, SIG_IGN); /* prevent a zombie */
#endif

	pid = fork();
	if ((pid_t) -1 == pid) {
		g_warning("adns_init: fork() failed: %s", g_strerror(errno));
		goto prefork_failure;
	}
	if (0 == pid) {
		/* child process */

		/**
		 * Close all standard FILEs so that they don't keep a reference
		 * to the log files when they are reopened by the main process
		 * on SIGHUP. This means there will be no visible messages from
		 * ADNS at all.
		 */

		if (!freopen("/dev/null", "r", stdin))
			g_error("adns_init: freopen(\"/dev/null\", \"r\", stdin) failed: "
					"%s", g_strerror(errno));

		if (!freopen("/dev/null", "a", stdout))
			g_error("adns_init: freopen(\"/dev/null\", \"a\", stdout) failed: "
					"%s", g_strerror(errno));

		if (!freopen("/dev/null", "a", stderr))
			g_error("adns_init: freopen(\"/dev/null\", \"a\", stderr) failed: "
					"%s", g_strerror(errno));

		close(fd_query[1]);
		close(fd_reply[0]);

		set_close_on_exec(fd_query[0]);
		set_close_on_exec(fd_reply[1]);

		adns_helper(fd_query[0], fd_reply[1]);
		g_assert_not_reached();
		_exit(EXIT_SUCCESS);
	}

	/* parent process */
	close(fd_query[0]);
	close(fd_reply[1]);
	
	fd_query[1] = get_non_stdio_fd(fd_query[1]);
	fd_reply[0] = get_non_stdio_fd(fd_reply[0]);
	
	adns_query_fd = fd_query[1];

	set_close_on_exec(adns_query_fd);
	set_close_on_exec(fd_reply[0]);
	socket_set_nonblocking(adns_query_fd);
	socket_set_nonblocking(fd_reply[0]);
	
	adns_reply_event_id = inputevt_add(fd_reply[0], INPUT_EVENT_RX,
							adns_reply_callback, NULL);
	/* FALL THROUGH */
prefork_failure:

	if (!adns_reply_event_id) {
		g_warning("Cannot use ADNS; DNS lookups may cause stalling");
		CLOSE_IF_VALID(fd_query[0]);
		CLOSE_IF_VALID(fd_query[1]);
		CLOSE_IF_VALID(fd_reply[0]);
		CLOSE_IF_VALID(fd_reply[1]);
	}

	adns_cache = adns_cache_init();
}

/**
 * @return TRUE on success, FALSE on failure.
 */
static gboolean
adns_send_request(const struct adns_request *req)
{
	gchar buf[sizeof *req];
	size_t size;
	ssize_t written;

	g_assert(req);

	if (!adns_reply_event_id || 0 != adns_query_event_id)
		return FALSE;

	g_assert(adns_query_fd >= 0);
	
	memcpy(buf, &req->common, sizeof req->common);
	size = sizeof req->common;
	{
		gconstpointer p;
		size_t n;
		
		if (req->common.reverse) {
			n = sizeof req->query.reverse;
			p = &req->query.reverse;
		} else {
			n = sizeof req->query.by_addr;
			p = &req->query.by_addr;
		}
		memcpy(&buf[size], p, n);
		size += n;
	}

	/*
	 * Try to write the query atomically into the pipe.
	 */

	written = write(adns_query_fd, buf, size);
	if (written == (ssize_t) -1) {
		if (!is_temporary_error(errno)) {
			g_warning("adns_resolve: write() failed: %s",
				g_strerror(errno));
			inputevt_remove(adns_reply_event_id);
			adns_reply_event_id = 0;
			CLOSE_IF_VALID(adns_query_fd);
			return FALSE;
		}
		written = 0;
	}

	g_assert(0 == adns_query_event_id);

	/*
	 * If not written fully, allocate a spill buffer and record
	 * callback that will write the remaining data when the pipe
	 * can absorb new data.
	 */

	if ((size_t) written < size) {
		adns_async_write_t *aq;
	   
		aq = adns_async_write_alloc(req, &buf[written], size - written);
		adns_query_event_id = inputevt_add(adns_query_fd, INPUT_EVENT_WX,
								adns_query_callback, aq);
	}

	return TRUE;
}

/**
 * Creates a DNS resolve query for ``hostname''.
 *
 * The given function ``user_callback'' (which MUST NOT be NULL)
 * will be invoked with the resolved IP address and ``user_data''
 * as its parameters. The IP address 0.0.0.0 i.e., ``(guint32) 0''
 * is used to indicate a failure. In case the hostname is given as
 * an IP string, it will be directly converted and the callback
 * immediately invoked. If the adns helper process is ``out of service''
 * the query will be resolved synchronously.
 *
 * @return TRUE if the resolution is asynchronous i.e., the callback
 * will be called AFTER adns_resolve() returned. If the resolution is
 * synchronous i.e., the callback was called BEFORE adns_resolve()
 * returned, adns_resolve() returns FALSE.
 */
gboolean
adns_resolve(const gchar *hostname, enum net_type net,
	adns_callback_t user_callback, gpointer user_data)
{
	struct adns_request req;
	struct adns_response ans;
	struct adns_query *query = &req.query.by_addr;
	struct adns_reply *reply = &ans.reply.by_addr;
	size_t hostname_len;
	host_addr_t addr;

	g_assert(NULL != hostname);
	g_assert(NULL != user_callback);

	req.common.user_callback = (void (*)(void)) user_callback;
	req.common.user_data = user_data;
	req.common.reverse = FALSE;
	ans.common = req.common;
	
	query->net = net;
	reply->hostname[0] = '\0';
	reply->addrs[0] = zero_host_addr;

	hostname_len = g_strlcpy(query->hostname, hostname, sizeof query->hostname);
	if (hostname_len >= sizeof query->hostname) {
		/* truncation detected */
		adns_invoke_user_callback(&ans);
		return FALSE; /* synchronous */
	}

	if (string_to_host_addr(hostname, NULL, &addr)) {
		reply->addrs[0] = addr;
		reply->addrs[1] = zero_host_addr;
		adns_invoke_user_callback(&ans);
		return FALSE; /* synchronous */
	}

	ascii_strlower(query->hostname, hostname);
	g_strlcpy(reply->hostname, query->hostname, sizeof reply->hostname);
	
	if (
		adns_cache_lookup(adns_cache, tm_time(), query->hostname,
			reply->addrs, G_N_ELEMENTS(reply->addrs))
	) {
		adns_invoke_user_callback(&ans);
		return FALSE; /* synchronous */
	}

	if (adns_send_request(&req))
		return TRUE; /* asynchronous */

	if (adns_reply_event_id)
		g_warning("adns_resolve: using synchronous resolution for \"%s\"",
			query->hostname);

	adns_fallback(&req);

	return FALSE; /* synchronous */
}

/**
 * Creates a DNS reverse lookup query for ``addr''. The given function
 * ``user_callback'' (which MUST NOT be NULL) will be invoked with
 * the resolved hostname and ``user_data'' as its parameters. If the lookup
 * failed, the callback will be invoked with ``hostname'' NULL. If the adns
 * helper process is ``out of service'' the query will be processed
 * synchronously.
 *
 * @return TRUE if the resolution is asynchronous i.e., the callback
 * will be called AFTER adns_reverse_lookup() returned. If the resolution is
 * synchronous i.e., the callback was called BEFORE adns_reverse_lookup()
 * returned, adns_reverse_lookup() returns FALSE.
 */
gboolean
adns_reverse_lookup(const host_addr_t addr,
	adns_reverse_callback_t user_callback, gpointer user_data)
{
	struct adns_request req;
	struct adns_reverse_query *query = &req.query.reverse;

	g_assert(user_callback);

	req.common.user_callback = (void (*)(void)) user_callback;
	req.common.user_data = user_data;
	req.common.reverse = TRUE;
	query->addr = addr;

	if (adns_send_request(&req))
		return TRUE; /* asynchronous */

	g_warning("adns_reverse_lookup: using synchronous resolution for \"%s\"",
		host_addr_to_string(query->addr));

	adns_fallback(&req);

	return FALSE; /* synchronous */
}

/**
 * Removes the callback and frees the cache.
 */
void
adns_close(void)
{
	if (adns_reply_event_id) {
		inputevt_remove(adns_reply_event_id);
		adns_reply_event_id = 0;
	}
	if (adns_query_event_id) {
		inputevt_remove(adns_query_event_id);
		adns_query_event_id = 0;
	}
	
	adns_cache_free(adns_cache);
	adns_cache = NULL;
}

/* vi: set ts=4 sw=4 cindent: */
