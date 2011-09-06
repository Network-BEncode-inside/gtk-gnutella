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
 * Statistics routines.
 *
 * @author Raphael Manfredi
 * @date 2004
 */

#include "common.h"

#ifdef I_MATH
#include <math.h>
#endif	/* I_MATH */

#include "stats.h"
#include "glib-missing.h"	/* For g_slist_delete_link() */
#include "halloc.h"
#include "walloc.h"
#include "override.h"		/* Must be the last header included */

/**
 * A one-dimension container (x).
 */
struct statx {
	GSList *data;			/**< Data points (value = double *) */
	int n;					/**< Amount of points */
	double sx;				/**< Sx: sum of all points */
	double sx2;				/**< Sx2: sum of the square of all points */
	gboolean no_data;		/**< Do not keep data, it is managed externally */
};

typedef enum op {
	STATS_OP_REMOVE = -1,
	STATS_OP_ADD = +1
} stats_op_t;

/**
 * Create one-dimension container.
 */
statx_t *
statx_make(void)
{
	statx_t *sx;

	WALLOC0(sx);
	return sx;
}

/**
 * Create one-dimension set of statistics with no data management.
 */
statx_t *
statx_make_nodata(void)
{
	statx_t *sx;

	WALLOC0(sx);
	sx->no_data = TRUE;
	return sx;
}

/**
 * Destroy one-dimension container.
 */
void
statx_free(statx_t *sx)
{
	statx_clear(sx);
	WFREE(sx);
}

/**
 * Clear container.
 */
void
statx_clear(statx_t *sx)
{
	GSList *l;

	for (l = sx->data; l; l = g_slist_next(l)) {
		double *vp = (double *) l->data;
		WFREE(vp);
	}
	gm_slist_free_null(&sx->data);

	sx->n = 0;
	sx->sx = 0.0;
	sx->sx2 = 0.0;
}

/**
 * Add/substract one data point.
 *
 * @param sx the container
 * @param val the value to add/remove
 * @param op the operation: STATS_OP_ADD or STATS_OP_REMOVE
 */
static void
statx_opx(statx_t *sx, double val, stats_op_t op)
{
	g_assert(op == STATS_OP_ADD || sx->n > 0);
	g_assert(op == STATS_OP_ADD || sx->data != NULL || sx->no_data);

	if (!sx->no_data) {
		if (op == STATS_OP_REMOVE) {
			GSList *l;

			/*
			 * If value is removed, it must belong to the data set.
			 */

			for (l = sx->data; l; l = g_slist_next(l)) {
				double *vp = (double *) l->data;
				double delta = *vp - val;

				if (ABS(delta) < 1e-56) {
					sx->data = g_slist_remove(sx->data, vp);
					WFREE(vp);
					break;
				}
			}

			g_assert(l != NULL);		/* Found it */
		} else {
			double *vp;

			WALLOC(vp);
			*vp = val;
			sx->data = g_slist_prepend(sx->data, vp);
		}
	}

	sx->n += op;
	sx->sx += op * val;
	sx->sx2 += op * val * val;
}

/**
 * Add data point to container.
 */
void
statx_add(statx_t *sx, double val)
{
	statx_opx(sx, val, STATS_OP_ADD);
}

/**
 * Remove data point from container.
 */
void
statx_remove(statx_t *sx, double val)
{
	statx_opx(sx, val, STATS_OP_REMOVE);
}

/**
 * Remove oldest data point from container.
 */
void
statx_remove_oldest(statx_t *sx)
{
	GSList *l;
	double val = 0;

	g_assert(!sx->no_data);
	g_assert(sx->n >= 0);
	g_assert((sx->n > 0) ^ (NULL == sx->data));

	if (sx->n < 1)
		return;

	/*
	 * Since we prepend new items to the list (for speed), we need to find
	 * the next to last item to delete the final item.
	 */

	for (l = sx->data; l; l = g_slist_next(l)) {
		GSList *next = g_slist_next(l);
		if (next == NULL) {
			/* Only one item in list, `l' points to it */
			double *vp = (double *) l->data;
			val = *vp;
			WFREE(vp);
			gm_slist_free_null(&sx->data);
			break;
		} else if (NULL == g_slist_next(next)) {
			/* The item after `l' is the last item of the list */
			double *vp = (double *) next->data;
			val = *vp;
			WFREE(vp);
			next = g_slist_delete_link(l, next);
			break;
		}
	}

	sx->n--;
	sx->sx -= val;
	sx->sx2 -= val * val;

	g_assert((sx->n > 0) ^ (NULL == sx->data));
}

/**
 * @return amount of data points.
 */
int
statx_n(const statx_t *sx)
{
	return sx->n;
}

/**
 * @return average of data points.
 */
double
statx_avg(const statx_t *sx)
{
	g_assert(sx->n > 0);

	return sx->sx / sx->n;
}

/**
 * @return the standard deviation of the data points.
 */
double
statx_sdev(const statx_t *sx)
{
	return sqrt(statx_var(sx));
}

/**
 * @return the variance of the data points.
 */
double
statx_var(const statx_t *sx)
{
	g_assert(sx->n > 1);

	return (sx->sx2 - (sx->sx * sx->sx) / sx->n) / (sx->n - 1);
}

/**
 * @return the standard error of the mean.
 */
double
statx_stderr(const statx_t *sx)
{
	return sqrt(statx_var(sx) / sx->n);
}

/**
 * @return an array of datapoints which can be freed via hfree() when done.
 */
double *
statx_data(const statx_t *sx)
{
	double *array;
	int i;
	GSList *l;

	g_assert(!sx->no_data);
	g_assert(sx->n > 0);

	array = halloc(sizeof(double) * sx->n);

	for (i = 0, l = sx->data; i < sx->n && l; l = g_slist_next(l), i++) {
		double *vp = (double *) l->data;
		array[i] = *vp;
	}

	return array;
}

/* vi: set ts=4: */
