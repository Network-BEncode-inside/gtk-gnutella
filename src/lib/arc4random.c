/*
 * Copyright (c) 1996, David Mazieres <dm@lcs.mit.edu>.
 *
 * Adaptated for inclusion in gtk-gnutella by Raphael Manfredi.
 * Copyright (c) 2010, 2012 Raphael Manfredi
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
 * ARC4 random number generator.
 *
 * The arc4random() function uses the key stream generator employed by the
 * ARC4 cipher, which uses 256 8-bit S-Boxes.  The S-Boxes can be in about
 * 2^1700 states.
 *
 * There is no need to call arc4random_stir() before using arc4random()
 * since initialization happens auto-magically.  The initial seed is collected
 * through entropy_collect(), which supplies 160 random bits.
 *
 * The arc4random_upto() routine has been added to David Mazieres's code
 * to provide uniformly distributed random numbers over a certain range.
 *
 * @author David Mazieres
 * @date 1996
 * @author Raphael Manfredi
 * @date 2010, 2012
 */

#include "common.h"

#include "log.h"
#include "pow2.h"

#ifndef HAS_ARC4RANDOM

/*
 * Arc4 random number generator for OpenBSD.
 * Copyright 1996 David Mazieres <dm@lcs.mit.edu>.
 *
 * Modification and redistribution in source and binary forms is
 * permitted provided that due credit is given to the author and the
 * OpenBSD project (for instance by leaving this copyright notice
 * intact).
 */

/*
 * This code is derived from section 17.1 of Applied Cryptography,
 * second edition, which describes a stream cipher allegedly
 * compatible with RSA Labs "RC4" cipher (the actual description of
 * which is a trade secret).  The same algorithm is used as a stream
 * cipher called "arcfour" in Tatu Ylonen's ssh package.
 *
 * Here the stream cipher has been modified always to include the time
 * when initializing the state.  That makes it impossible to
 * regenerate the same random sequence twice, so this can't be used
 * for encryption, but will generate good random numbers.
 *
 * RC4 is a registered trademark of RSA Laboratories.
 */

#include "arc4random.h"
#include "entropy.h"
#include "misc.h"		/* For sha1_t */

struct arc4_stream {
	uint8 i;
	uint8 j;
	uint8 s[256];
};

static struct arc4_stream rs;
static bool rs_initialized;
static bool rs_stired;

static inline uint8 arc4_getbyte(struct arc4_stream *);
static void arc4_stir(struct arc4_stream *);

static inline void
arc4_init(struct arc4_stream *as)
{
	int n;

	for (n = 0; n < 256; n++)
		as->s[n] = n;
	as->i = 0;
	as->j = 0;
}

static inline void
arc4_addrandom(struct arc4_stream *as, const unsigned char *dat, int datlen)
{
	int n;
	uint8 si;

	as->i--;
	for (n = 0; n < 256; n++) {
		as->i = (as->i + 1);
		si = as->s[as->i];
		as->j = (as->j + si + dat[n % datlen]);
		as->s[as->i] = as->s[as->j];
		as->s[as->j] = si;
	}
}

static inline void
arc4_check_init(void)
{
	if G_UNLIKELY(!rs_initialized) {
		arc4_init(&rs);
		rs_initialized = TRUE;
	}
}

static void
arc4_stir(struct arc4_stream *as)
{
	int n;

	arc4_check_init();

	/*
	 * Collect 1024 bytes of initial entropy: the more randomness there
	 * is in the initial state, the more random combinations we can produce
	 * after initialization.
	 */

	for (n = 0; n < 4; n++) {
		unsigned char buf[256];		/* Optimal size for arc4_addrandom() */

		entropy_fill(buf, sizeof buf);
		arc4_addrandom(as, buf, sizeof buf);
	}

	/*
	 * Throw away the first N bytes of output, as suggested in the
	 * paper "Weaknesses in the Key Scheduling Algorithm of RC4"
	 * by Fluher, Mantin, and Shamir.  N=1024 is based on
	 * suggestions in the paper "(Not So) Random Shuffles of RC4"
	 * by Ilya Mironov.
	 */

	for (n = 0; n < 1024; n++)
		arc4_getbyte(as);
}

static inline G_GNUC_HOT uint8
arc4_getbyte(struct arc4_stream *as)
{
	uint8 si, sj;

	as->i = (as->i + 1);
	si = as->s[as->i];
	as->j = (as->j + si);
	sj = as->s[as->j];
	as->s[as->i] = sj;
	as->s[as->j] = si;

	return (as->s[(si + sj) & 0xff]);
}

static inline G_GNUC_HOT uint32
arc4_getword(struct arc4_stream *as)
{
	uint32 val;

	val = arc4_getbyte(as) << 24;
	val |= arc4_getbyte(as) << 16;
	val |= arc4_getbyte(as) << 8;
	val |= arc4_getbyte(as);

	return (val);
}

static inline ALWAYS_INLINE void
arc4_check_stir(void)
{
	if G_UNLIKELY(!rs_stired) {
		arc4_stir(&rs);
		rs_stired = TRUE;
	}
}

/**
 * Collect random entropy and add it to the random pool.
 *
 * This routine can be optionally called to refresh the random pool.
 * It is otherwise automatically called if not done before, at the first
 * attempt to get random numbers.
 */
void
arc4random_stir(void)
{
	/* THREAD_LOCK(); */
	arc4_stir(&rs);
	/* THREAD_UNLOCK(); */
}

/**
 * Perform random initialization if not already done.
 */
G_GNUC_COLD void
arc4random_stir_once(void)
{
	arc4_check_stir();
}

/**
 * Supply additional randomness to the pool.
 *
 * The optimal buffer length is 256 bytes.  Any larger size will cause
 * some bytes to be ignored (which is sub-optimal), whilst any smaller
 * size will cause bytes to be reused during the internal state shuffle
 * (which is OK).
 *
 * @param dat		pointer to a buffer containing random data
 * @param datlen	length of the buffer
 */
void
arc4random_addrandom(const unsigned char *dat, int datlen)
{
	g_assert(dat != NULL);
	g_assert(datlen > 0);

	/* THREAD_LOCK(); */
	arc4_check_stir();
	arc4_addrandom(&rs, dat, datlen);
	/* THREAD_UNLOCK(); */
}

/**
 * @return a new 32-bit random number.
 */
G_GNUC_HOT uint32
arc4random(void)
{
	uint32 rnd;

	/* THREAD_LOCK(); */
	arc4_check_stir();
	rnd = arc4_getword(&rs);
	/* THREAD_UNLOCK(); */

	return rnd;
}

#else	/* HAS_ARC4RANDOM */

/**
 * Perform random initialization if not already done.
 *
 * @attention
 * This is a non-standard call, specific to this library.
 */
G_GNUC_COLD void
arc4random_stir_once(void)
{
	static int done;

	if G_UNLIKELY(!done) {
		arc4random_stir();
		done = TRUE;
	}
}

#endif	/* !HAS_ARC4RANDOM */

/**
 * @return uniformly distributed random number in the [0, max] range.
 */
uint32
arc4random_upto(uint32 max)
{
	uint32 range, min, value;

	if G_UNLIKELY(0 == max)
		return 0;

	if G_UNLIKELY((uint32) -1 == max)
		return arc4random();

	/*
	 * We can't just use the modulo operator blindly because that would
	 * create a small bias: if the 2^32 interval cannot be evenly divided
	 * by the range of the random values, the fraction of the numbers that
	 * lie in the trailing partial fragment will be more likely to occur
	 * than others.  The larger the range, the higher the bias.
	 *
	 * Imagine we throw a 10-sided dice, getting values from 0 to 9.
	 * If we want only a random number in the [0, 2] interval, then we
	 * cannot just return the value of d10 % 3: looking at the number of
	 * results that can produce the resulting random number, we see that:
	 *
	 *   0 is produced by 0, 3, 6, 9
	 *   1 is produced by 1, 4, 7
	 *   2 is produced by 2, 5, 8
	 *
	 * So 0 has 40% chances of being returned, with 1 and 2 having 30%.
	 * A uniform distribution would require 33.33% chances for each number!
	 *
	 * If the range is a power of 2, then we know the 2^32 interval can be
	 * evenly divided and we can just mask the lower bits (all bits are random
	 * in an arc4random() value).
	 *
	 * Otherwise, we have to exclude values from the set of possible 2^32
	 * values to restore a uniform probability for all outcomes.  In our d10
	 * example above, removing the 0 value (i.e. rethrowing the d10 when we
	 * get a 0) restores the 33.33% chances for each number to be produced).
	 *
	 * The amount of values to exclude is (2^32 % range).
	 */

	range = max + 1;

	if (is_pow2(range))
		return arc4random() & (range - 1);

	/*
	 * Compute the minimum value we need in the 2^32 range to restore
	 * uniform probability for all outcomes.
	 *
	 * We want to exclude the first (2^32 % range) values.
	 */

	if (range > (1U << 31)) {
		min = ~range + 1;		/* 2^32 - range */
	} else {
		/*
		 * Can't represent 2^32 in 32 bits, so we use an alternate computation.
		 *
		 * Because range <= 2^31, we can compute (2^32 - range) % range, and
		 * it will yield the same result as 2^32 % range: (Z/nZ, +) is a group,
		 * and range % range = 0.
		 *
		 * To compute (2^32 - range) without using 2^32, we cheat, realizing
		 * that 2^32 = -1 + 1 (using unsigned arithmetic).
		 */

		min = ((uint32) -1 - range + 1) % range;
	}

	value = arc4random();

	if G_UNLIKELY(value < min) {
		size_t i;

		for (i = 0; i < 100; i++) {
#ifdef HAS_ARC4RANDOM
			value = arc4random();
#else
			/* THREAD_LOCK(); */
			/* All bytes are random anyway, just drop the first one */
			value = (value << 8) | arc4_getbyte(&rs);
			/* THREAD_UNLOCK(); */
#endif	/* HAS_ARC4RANDOM */

			if (value >= min)
				goto done;
		}

		/* Will occur once every 10^30 attempts */
		s_error("no luck with random number generator");
	}

done:
	return value % range;
}

/**
 * @return 64-bit random number.
 */
static inline uint64
arc4random64(void)
{
	return ((uint64) arc4random() << 32) | (uint64) arc4random();
}

/**
 * @return uniformly distributed 64-bit random number in the [0, max] range.
 */
uint64
arc4random_upto64(uint64 max)
{
	uint64 range, min, value;

	if G_UNLIKELY(0 == max)
		return 0;

	if G_UNLIKELY((uint64) -1 == max)
		return arc4random64();

	range = max + 1;

	if (IS_POWER_OF_2(range))
		return arc4random64() & (range - 1);

	/*
	 * Same logic as arc4random_upto() but in 64-bit arithmetic.
	 */

	if (range > ((uint64) 1U << 63)) {
		min = ~range + 1;		/* 2^64 - range */
	} else {
		min = ((uint64) -1 - range + 1) % range;
	}

	value = arc4random64();

	if G_UNLIKELY(value < min) {
		size_t i;

		for (i = 0; i < 100; i++) {
#ifdef HAS_ARC4RANDOM
			value = arc4random64();
#else
			/* THREAD_LOCK(); */
			/* All bytes are random anyway, just drop the first one */
			value = (value << 8) | (uint64) arc4_getbyte(&rs);
			/* THREAD_UNLOCK(); */
#endif	/* HAS_ARC4RANDOM */

			if (value >= min)
				goto done;
		}

		/* Will occur once every 10^30 attempts */
		s_error("no luck with random number generator");
	}

done:
	return value % range;
}

/* vi: set ts=4 sw=4 cindent: */
