/*
 * Copyright (c) 2002-2003 Richard Eckart
 * Copyright (c) 2013 Raphael Manfredi
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
 * Event mangement & dispatching logic.
 *
 * @author Richard Eckart
 * @date 2002-2003
 * @author Raphael Manfredi
 * @date 2013
 */

#include "common.h"

#include "event.h"
#include "hikset.h"
#include "misc.h"
#include "omalloc.h"
#include "spinlock.h"
#include "stacktrace.h"
#include "walloc.h"

#include "override.h"		/* Must be the last header included */

static inline struct subscriber *
subscriber_new(callback_fn_t cb, enum frequency_type t, uint32 interval)
{
    struct subscriber *s;

    g_assert(cb != NULL);

    WALLOC0(s);
    s->cb = cb;
    s->f_type = t;
    s->f_interval = interval;

    return s;
}

static inline void
subscriber_destroy(struct subscriber *s)
{
	WFREE(s);
}

/**
 * Allocate a new event identified by its name (static data not copied).
 *
 * @return allocated event structure, never meant to be freed.
 */
struct event *
event_new(const char *name)
{
    struct event *evt;

    g_assert(name != NULL);

    evt = omalloc0(sizeof *evt);
    evt->name = name;
	spinlock_init(&evt->lock);

    return evt;		/* Allocated once, never freed */
}

/**
 * Destroy an event and free all associated memory. The pointer to the
 * event will be NULL after this call.
 */
void
event_destroy(struct event *evt)
{
    GSList *sl;

	spinlock(&evt->lock);

    for (sl = evt->subscribers; sl; sl = g_slist_next(sl))
        subscriber_destroy(sl->data);

	g_slist_free(evt->subscribers);
	evt->subscribers = NULL;
	evt->destroyed = TRUE;

	spinunlock(&evt->lock);

	/* Event not freed, allocated via omalloc() */
}

void
event_add_subscriber(struct event *evt, callback_fn_t cb,
	enum frequency_type t, uint32 interval)
{
    struct subscriber *s;
	GSList *sl;

    g_assert(evt != NULL);
    g_assert(cb != NULL);
	g_assert(!evt->destroyed);

    s = subscriber_new(cb, t, interval);

	spinlock(&evt->lock);
	for (sl = evt->subscribers; sl; sl = g_slist_next(sl)) {
		struct subscriber *sb = sl->data;
		g_assert(sb != NULL);

		g_assert_log(sb->cb != cb,
			"%s(): attempt to add callback %s() twice",
			G_STRFUNC, stacktrace_function_name(cb));
	}

    evt->subscribers = g_slist_prepend(evt->subscribers, s);
	spinunlock(&evt->lock);
}

void
event_remove_subscriber(struct event *evt, callback_fn_t cb)
{
    GSList *sl;
	struct subscriber *s = NULL;

    g_assert(evt != NULL);
    g_assert(cb != NULL);

	spinlock(&evt->lock);

	if G_UNLIKELY(evt->destroyed) {
		/*
		 * Event was destroyed, all subcribers were already removed.
		 */

		spinunlock(&evt->lock);
		return;	
	}

	for (sl = evt->subscribers; sl; sl = g_slist_next(sl)) {
			s = sl->data;
			g_assert(s != NULL);
			if G_UNLIKELY(s->cb == cb)
				goto found;
	}

	g_error("%s(): attempt to remove unknown callback %s()",
		G_STRFUNC, stacktrace_function_name(cb));

found:
	g_assert(s->cb == cb);

    evt->subscribers = g_slist_remove(evt->subscribers, s);
	spinunlock(&evt->lock);

	subscriber_destroy(s);
}

uint
event_subscriber_count(struct event *evt)
{
	uint len;

	spinlock(&evt->lock);
	len = g_slist_length(evt->subscribers);
	spinunlock(&evt->lock);

	return len;
}

bool
event_subscriber_active(struct event *evt)
{
	return NULL != evt->subscribers;
}

struct event_table *
event_table_new(void)
{
    struct event_table *t;

	WALLOC0(t);
	t->events = hikset_create(offsetof(struct event, name), HASH_KEY_STRING, 0);
	spinlock_init(&t->lock);

	return t;
}

void
event_table_destroy(struct event_table *t, bool cleanup)
{
	spinlock(&t->lock);

    if (cleanup)
        event_table_remove_all(t);

    hikset_free_null(&t->events);
	spinlock_destroy(&t->lock);
	WFREE(t);
}

void
event_table_add_event(struct event_table *t, struct event *evt)
{
    g_assert(t != NULL);
    g_assert(evt != NULL);

    g_assert(t->events != NULL);
    g_assert(!hikset_contains(t->events, evt->name));

	spinlock(&t->lock);
    hikset_insert_key(t->events, &evt->name);
	spinunlock(&t->lock);
}

void
event_table_remove_event(struct event_table *t, struct event *evt)
{
    g_assert(t != NULL);
    g_assert(evt != NULL);

    g_assert(t->events != NULL);
    g_assert(hikset_contains(t->events, evt->name));

	spinlock(&t->lock);
    hikset_remove(t->events, evt->name);
	spinunlock(&t->lock);
}

static void
clear_helper(void *value, void *unused_data)
{
	(void) unused_data;
    event_destroy(value);
}

void
event_table_remove_all(struct event_table *t)
{
    g_assert(t != NULL);
    g_assert(t->events != NULL);

	spinlock(&t->lock);
    hikset_foreach(t->events, clear_helper, NULL);
	hikset_clear(t->events);
	spinunlock(&t->lock);
}

/* vi: set ts=4 sw=4 cindent: */
