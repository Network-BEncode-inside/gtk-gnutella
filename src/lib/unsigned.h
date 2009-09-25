/*
 * $Id$
 *
 * Copyright (c) 2008-2009, Raphael Manfredi
 * Copyright (c) 2006-2008, Christian Biere
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
 * Unsigned quantity arithmetic and comparisons.
 *
 * @author Raphael Manfredi
 * @date 2008-2009
 * @author Christian Biere
 * @date 2006-2008
 */

#ifndef _unsigned_h_
#define _unsigned_h_

/*
 * NOTE: ssize_t is NOT the signed variant of size_t and casting values blindly
 * to ssize_t may cause integer overflows.  Larger values, especially SIZE_MAX
 * (size_t)-1 may be the result of errors or wrap arounds during calculations.
 * Therefore in places where memory objects larger than half of the address
 * space are unreasonable, the following two functions are useful to check for
 * such conditions.
 */

/*
 * Check whether a signed representation of size would be non-negative.
 * @return TRUE if size is equal to zero or larger and smaller than
 *         SIZE_MAX / 2.
 */
static inline gboolean
size_is_non_negative(size_t size)
{
	return size <= SIZE_MAX / 2;
}

/**
 * Check whether a signed representation of size would be strictly positive.
 * @return TRUE if size is larger than zero and smaller than SIZE_MAX / 2.
 */
static inline gboolean
size_is_positive(size_t size)
{
	return size_is_non_negative(size - 1);
}

/*
 * Calculate the sum of a and b but saturate towards SIZE_MAX.
 * @return SIZE_MAX if a + b > SIZE_MAX, otherwise a + b.
 */
static inline size_t
size_saturate_add(size_t a, size_t b)
{
	size_t ret = a + b;
	if (G_UNLIKELY(ret < a))
		return SIZE_MAX;
	return ret;
}

/*
 * Calculate the product of a and b but saturate towards SIZE_MAX.
 * @return SIZE_MAX if a * b > SIZE_MAX, otherwise a * b.
 */
static inline size_t
size_saturate_mult(size_t a, size_t b)
{
	if (0 == a)
		return 0;
	if (G_UNLIKELY(SIZE_MAX / a < b))
		return SIZE_MAX;
	return a * b;
}

/*
 * Calculate the difference between a and b but saturate towards zero.
 * @return zero if a < b, otherwise a - b.
 */
static inline size_t
size_saturate_sub(size_t a, size_t b)
{
	if (G_UNLIKELY(a < b))
		return 0;
	return a - b;
}

/**
 * Check whether a signed representation of unsigned int would be non-negative.
 * @return TRUE if size is greater than or equal to zero, yet smaller than the
 * maximum positive quantity that can be represented.
 */
static inline gboolean
uint_is_non_negative(unsigned v)
{
	return v <= MAX_INT_VAL(unsigned) / 2;
}

/**
 * Check whether a signed representation of value would be strictly positive.
 * @return TRUE if size is stricly larger than zero, yet smaller than the
 * maximum positive quantity that can be represented.
 */
static inline gboolean
uint_is_positive(unsigned v)
{
	return uint_is_non_negative(v - 1);
}

/**
 * Check whether a signed representation of value would be non-negative.
 * @return TRUE if size is greater than or equal to zero, yet smaller than the
 * maximum positive quantity that can be represented.
 */
static inline gboolean
guint32_is_non_negative(guint32 v)
{
	return v <= MAX_INT_VAL(guint32) / 2;
}

/**
 * Check whether a signed representation of value would be strictly positive.
 * @return TRUE if size is stricly larger than zero, yet smaller than the
 * maximum positive quantity that can be represented.
 */
static inline gboolean
guint32_is_positive(guint32 v)
{
	return guint32_is_non_negative(v - 1);
}

/*
 * Calculate the sum of a and b but saturate towards the maximum value.
 * @return maximum if a + b > maximum, otherwise a + b.
 */
static inline guint32
guint32_saturate_add(guint32 a, guint32 b)
{
	guint32 ret = a + b;
	if (G_UNLIKELY(ret < a))
		return MAX_INT_VAL(guint32);
	return ret;
}

#endif /* _unsigned_h_ */

/* vi: set ts=4 sw=4 cindent: */
