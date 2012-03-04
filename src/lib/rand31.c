/*
 * Copyright (c) 2012 Raphael Manfredi
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
 * Simple Pseudo-Random Number Generation (PRNG) engine.
 *
 * This engine should not be used when strong random numbers are necessary.
 * It is only meant to produce a simple source of randomness for tests, or
 * to bootstrap stronger engines.
 *
 * It generates 31-bit random numbers via an interface compatible with the
 * rand() C library function, only we append "31" at the tail to distinguish
 * rand31() from the standard routine, and also emphasize the limited range
 * of the random numbers we produce.
 *
 * When a sequence of random numbers yields an interesting result, it is
 * possible to replay it by querying the original random seed.  The only catch
 * is that the number 0 is not a valid seed for rand31_set_seed(), as it
 * requests the computation of a new random seed.  Given this PRNG is only
 * meant for tests and not for serious random numbers, this is not deemed a
 * problem, only bad luck.
 *
 * To (slowly) generate strong random numbers, use entropy_random().
 * For (faster) strong random nubers, use arc4random().
 *
 * @author Raphael Manfredi
 * @date 2012
 */

#include "common.h"

#include "rand31.h"
#include "entropy.h"
#include "hashing.h"
#include "log.h"
#include "mempcpy.h"
#include "pow2.h"
#include "stacktrace.h"
#include "tm.h"

#include "override.h"			/* Must be the last header included */

static bool rand31_seeded;			/**< Whether PRNG was seeded */
static unsigned rand31_seed;		/**< The current seed */
static unsigned rand31_first_seed;	/**< The initial seed */

/**
 * @return next random number following given seed.
 */
static inline unsigned
rand31_prng_next(unsigned seed)
{
	return (seed * 1103515245 + 12345) & RAND31_MASK;
}

/**
 * Computes a random seed to initialize the PRNG engine.
 *
 * @return initial random seed.
 */
static unsigned
rand31_random_seed(void)
{
	tm_t now;
	double cpu;
	jmp_buf env;
	unsigned discard;
	unsigned seed;

	/*
	 * Our simple PRNG has only 31 bits of internal state.
	 *
	 * It is seeded by hashing some environmental constants: the ID of
	 * the process, the current time and current CPU state.  To further
	 * create a unique starting point in the series of generated numbers,
	 * a second different hashing is done and reduced to 8 bits.  This
	 * is interpreted as the amount of initial random values to discard.
	 */

	cpu = tm_cputime(NULL, NULL);
	tm_now_exact(&now);
	seed = (GOLDEN_RATIO_31 * getpid()) >> 1;
	seed += binary_hash(&now, sizeof now);
	seed += binary_hash(&cpu, sizeof cpu);
	entropy_delay();
	tm_now_exact(&now);
	seed += binary_hash(&now, sizeof now);
	ZERO(&env);			/* Avoid uninitialized memory reads */
	if (setjmp(env)) {
		g_assert_not_reached(); /* We never longjmp() */
	}
	seed += binary_hash(env, sizeof env);
	discard = binary_hash2(env, sizeof env);
	discard ^= binary_hash2(&now, sizeof now);
	discard += getpid();
	cpu = tm_cputime(NULL, NULL);
	discard += binary_hash2(&cpu, sizeof cpu);
	discard = hashing_fold(discard, 8);
	while (0 != discard--) {
		seed = rand31_prng_next(seed);
	}

	return seed;
}

/**
* Linear congruential pseudo-random number generation (PRNG).
*
* This PRNG is not used directly but rather through rand31().
*
* @return a 31-bit random number.
*/
static unsigned
rand31_prng(void)
{
	if G_UNLIKELY(!rand31_seeded)
		rand31_set_seed(0);

	return rand31_seed = rand31_prng_next(rand31_seed);
}

/**
 * Minimal pseudo-random number generation, combining a simple PRNG with
 * past-collected entropy.
 *
 * @return a 31-bit (positive) random number.
 */
int
rand31(void)
{
	/*
	 * The low-order bits of the PRNG are less random than the upper ones,
	 * and they have a smaller period.  Keep only the leading 16 bits of the
	 * first value and the leading 15 bits of the second value.
	 */

	return (rand31_prng() >> 15) | (rand31_prng() & 0x7fff0000);
}

/**
 * Initialize the random seed.
 *
 * Using a seed of 0 computes a new random seed.
 */
void
rand31_set_seed(unsigned seed)
{
	rand31_first_seed = rand31_seed = 0 == seed ? rand31_random_seed() : seed;
	rand31_seeded = TRUE;
}

/**
 * @return initial seed, to be able to reproduce a random sequence.
 */
unsigned
rand31_initial_seed(void)
{
	return rand31_first_seed;
}

/**
 * Compute uniformly distributed random number in the [0, max] range,
 * avoiding any modulo bias, using the specified random function to generate
 * the numbers.
 *
 * @param fn	function generating 31-bit wide numbers
 * @param max	maximum allowed value for the result (inclusive)
 *
 * @return uniformly distributed 31-bit number from 0 to max, inclusive.
 */
int
rand31_upto(rand31_fn_t rf, unsigned max)
{
	unsigned range, min;
	int i;

	g_assert(rf != NULL);
	g_assert(max <= INT_MAX);

	if G_UNLIKELY(0 == max)
		return 0;

	if G_UNLIKELY(INT_MAX == max)
		return (*rf)();

	/*
	 * See arc4random_upto() for details on modulo bias and how our
	 * strategy restores a uniform distribution.
	 *
	 * The code here is simpler because there cannot be any overflow on
	 * the 32-bit unsigned value.
	 */

	range = max + 1;

	if (is_pow2(range))
		return (*rf)() & (range - 1);

	min = (1U << 31) % range;

	for (i = 0; i < 100; i++) {
		unsigned value = (*rf)();

		if (value >= min)
			return value % range;
	}

	s_error("no luck with random number generator %s()",
		stacktrace_routine_name(func_to_pointer(rf), FALSE));
}

/**
 * Compute uniformly distributed random number in the [0, max] range,
 * avoiding any modulo bias.
 *
 * @return uniformly distributed 31-bit number from 0 to max, inclusive.
 */
int
rand31_value(unsigned max)
{
	return rand31_upto(rand31, max);
}

/**
 * Build a 32-bit random number.
 */
static inline uint32
rand31_u32(void)
{
	return (rand31() << 5) + (rand31_prng() >> 15);
}

/**
 * Fills buffer 'dst' with 'size' bytes of random data.
 */
void
rand31_bytes(void *dst, size_t size)
{
	char *p = dst;

	while (size > 4) {
		const uint32 value = rand31_u32();
		p = mempcpy(p, &value, 4);
		size -= 4;
	}
	if (size > 0) {
		const uint32 value = rand31_u32();
		memcpy(p, &value, size);
	}
}

/* vi: set ts=4 sw=4 cindent: */
