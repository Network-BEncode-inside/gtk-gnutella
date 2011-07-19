/*
 * $Id$
 *
 * Copyright (c) 2006-2009, Raphael Manfredi
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
 * @ingroup dht
 * @file
 *
 * Kademlia Unique IDs (KUID) and KUID-based integer arithmetic.
 *
 * @author Raphael Manfredi
 * @date 2006-2009
 */

#ifndef _dht_kuid_h_
#define _dht_kuid_h_

#include "if/dht/kuid.h"

/*
 * Public interface.
 */

void kuid_random_fill(kuid_t *kuid);
void kuid_from_buf(kuid_t *dest, const gchar *id);
gboolean kuid_is_blank(const kuid_t *kuid) G_GNUC_PURE;
int kuid_cmp3(const kuid_t *target, const kuid_t *kuid1, const kuid_t *kuid2)
	G_GNUC_PURE;
int kuid_cmp(const kuid_t *kuid1, const kuid_t *kuid2) G_GNUC_PURE;
void kuid_xor_distance(kuid_t *res, const kuid_t *k1, const kuid_t *k2);
gboolean kuid_match_nth(const kuid_t *k1, const kuid_t *k2, int bits)
	G_GNUC_PURE;
size_t kuid_common_prefix(const kuid_t *k1, const kuid_t *k2) G_GNUC_PURE;
void kuid_random_within(kuid_t *dest, const kuid_t *prefix, int bits);
void kuid_flip_nth_leading_bit(kuid_t *res, int n);

void kuid_zero(kuid_t *res);
void kuid_not(kuid_t *k);
void kuid_set32(kuid_t *res, guint32 val);
void kuid_set64(kuid_t *res, guint64 val);
void kuid_set_nth_bit(kuid_t *res, int n);
gboolean kuid_add(kuid_t *res, const kuid_t *other);
gboolean kuid_add_u8(kuid_t *k, guint8 l);
gboolean kuid_lshift(kuid_t *res);
void kuid_rshift(kuid_t *res);
guint8 kuid_mult_u8(kuid_t *res, guint8 l);
void kuid_divide(const kuid_t *k1, const kuid_t *k2, kuid_t *q, kuid_t *r);
double kuid_to_double(const kuid_t *value);
guint64 kuid_to_guint64(const kuid_t *value);

/**
 * Return leading KUID byte.
 */
static inline guint8
kuid_leading_u8(const kuid_t *k)
{
	return k->v[0];
}

#endif /* _dht_kuid_h_ */

/* vi: set ts=4 sw=4 cindent: */
