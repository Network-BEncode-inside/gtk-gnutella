/*
 * sort-test -- sort tests and benchmarking.
 *
 * Copyright (c) 2012 Raphael Manfredi <Raphael_Manfredi@pobox.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the authors nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "common.h"

#include "lib/base16.h"
#include "lib/rand31.h"
#include "lib/smsort.h"
#include "lib/str.h"
#include "lib/tm.h"
#include "lib/xmalloc.h"
#include "lib/xsort.h"

#define TEST_BITS	16
#define TEST_WORDS	4

#define DUMP_BYTES	16

char *progname;
size_t item_size;

typedef void (*xsort_routine)(void *b, size_t n, size_t s, xsort_cmp_t cmp);

static void G_GNUC_NORETURN
usage(void)
{
	fprintf(stderr,
		"Usage: %s [-ht] [-c items] [-n loops] [-s item_size] [-R seed]\n"
		"  -c : sets item count to test\n"
		"  -h : prints this help message\n"
		"  -n : sets amount of loops\n"
		"  -s : sets item size to test, in bytes\n"
		"  -t : time each test\n"
		"  -R : seed for repeatable random key sequence\n"
		, progname);
	exit(EXIT_FAILURE);
}

typedef int (*cmp_routine)(const void *a, const void *b);

static int
long_cmp(const void *a, const void *b)
{
	const ulong *la = a, *lb = b;
	const ulong va = *la, vb = *lb;

	return CMP(va, vb);
}

static int
long_revcmp(const void *a, const void *b)
{
	const ulong *la = a, *lb = b;
	const ulong va = *la, vb = *lb;

	return CMP(vb, va);
}

static int
generic_cmp(const void *a, const void *b)
{
	return memcmp(a, b, item_size);	/* Global variable */
}

static int
generic_revcmp(const void *a, const void *b)
{
	return memcmp(b, a, item_size);	/* Global variable */
}

static cmp_routine
get_cmp_routine(size_t isize)
{
	switch (isize) {
	case LONGSIZE:
		return long_cmp;
	default:
		item_size = isize;	/* Global variable */
		return generic_cmp;
	}
}

static cmp_routine
get_revcmp_routine(size_t isize)
{
	switch (isize) {
	case LONGSIZE:
		return long_revcmp;
	default:
		item_size = isize;	/* Global variable */
		return generic_revcmp;
	}
}

struct plain {
	char val[LONGSIZE];
};

struct plain_1 {
	char val[LONGSIZE];
	char buf[INTSIZE];
};

struct plain_2 {
	char val[LONGSIZE];
	char buf[INTSIZE * 2];
};

struct plain_3 {
	char val[LONGSIZE];
	char buf[INTSIZE * 3];
};

struct plain_4 {
	char val[LONGSIZE];
	char buf[INTSIZE * 4];
};

static bool
plain_less(void *m, size_t i, size_t j)
{
	struct plain *x = m;
	struct plain *a = &x[i];
	struct plain *b = &x[j];

	return long_cmp(&a->val, &b->val) < 0;
}

static bool
plain_1_less(void *m, size_t i, size_t j)
{
	struct plain_1 *x = m;
	struct plain_1 *a = &x[i];
	struct plain_1 *b = &x[j];
	int c;

	c = memcmp(&a->val, &b->val, sizeof a->val);
	if (0 == c)
		return memcmp(&a->buf, &b->buf, sizeof a->buf) < 0;
	return c < 0;
}

static bool
plain_2_less(void *m, size_t i, size_t j)
{
	struct plain_2 *x = m;
	struct plain_2 *a = &x[i];
	struct plain_2 *b = &x[j];
	int c;

	c = memcmp(&a->val, &b->val, sizeof a->val);
	if (0 == c)
		return memcmp(&a->buf, &b->buf, sizeof a->buf) < 0;
	return c < 0;
}

static bool
plain_3_less(void *m, size_t i, size_t j)
{
	struct plain_3 *x = m;
	struct plain_3 *a = &x[i];
	struct plain_3 *b = &x[j];
	int c;

	c = memcmp(&a->val, &b->val, sizeof a->val);
	if (0 == c)
		return memcmp(&a->buf, &b->buf, sizeof a->buf) < 0;
	return c < 0;
}

static bool
plain_4_less(void *m, size_t i, size_t j)
{
	struct plain_4 *x = m;
	struct plain_4 *a = &x[i];
	struct plain_4 *b = &x[j];
	int c;

	c = memcmp(&a->val, &b->val, sizeof a->val);
	if (0 == c)
		return memcmp(&a->buf, &b->buf, sizeof a->buf) < 0;
	return c < 0;
}

static bool
generic_less(void *m, size_t i, size_t j)
{
	void *a = ptr_add_offset(m, i * item_size);	/* Global variable */
	void *b = ptr_add_offset(m, j * item_size);	/* Global variable */

	return memcmp(a, b, item_size) < 0;	/* Global variable */
}


static smsort_less_t
get_less_routine(size_t isize)
{
	if (sizeof(struct plain) == isize)
		return plain_less;
	else if (sizeof(struct plain_1) == isize)
		return plain_1_less;
	else if (sizeof(struct plain_2) == isize)
		return plain_2_less;
	else if (sizeof(struct plain_3) == isize)
		return plain_3_less;
	else if (sizeof(struct plain_4) == isize)
		return plain_4_less;
	else {
		item_size = isize;		/* Global variable */
		return generic_less;
	}
}

static void
plain_swap(void *m, size_t i, size_t j)
{
	struct plain *x = m;
	struct plain tmp;

	tmp = x[j];
	x[j] = x[i];
	x[i] = tmp;
}

static void
plain_1_swap(void *m, size_t i, size_t j)
{
	struct plain_1 *x = m;
	struct plain_1 tmp;

	tmp = x[j];
	x[j] = x[i];
	x[i] = tmp;
}

static void
plain_2_swap(void *m, size_t i, size_t j)
{
	struct plain_2 *x = m;
	struct plain_2 tmp;

	tmp = x[j];
	x[j] = x[i];
	x[i] = tmp;
}

static void
plain_3_swap(void *m, size_t i, size_t j)
{
	struct plain_3 *x = m;
	struct plain_3 tmp;

	tmp = x[j];
	x[j] = x[i];
	x[i] = tmp;
}

static void
plain_4_swap(void *m, size_t i, size_t j)
{
	struct plain_4 *x = m;
	struct plain_4 tmp;

	tmp = x[j];
	x[j] = x[i];
	x[i] = tmp;
}

static void
generic_swap(void *m, size_t i, size_t j)
{
	void *a = ptr_add_offset(m, i * item_size);	/* Global variable */
	void *b = ptr_add_offset(m, j * item_size);	/* Global variable */

	SWAP(a, b, item_size);	/* Global variable */
}

static smsort_swap_t
get_swap_routine(size_t isize)
{
	if (sizeof(struct plain) == isize)
		return plain_swap;
	else if (sizeof(struct plain_1) == isize)
		return plain_1_swap;
	else if (sizeof(struct plain_2) == isize)
		return plain_2_swap;
	else if (sizeof(struct plain_3) == isize)
		return plain_3_swap;
	else if (sizeof(struct plain_4) == isize)
		return plain_4_swap;
	else
		return generic_swap;
}

static void
xtest(xsort_routine f, void *array, void *copy,
	size_t cnt, size_t isize, size_t loops)
{
	cmp_routine cmp = get_cmp_routine(isize);
	size_t len = cnt * isize;

	do {
		memcpy(copy, array, len);
		(*f)(copy, cnt, isize, cmp);
	} while (--loops > 0);
}

static void
xsort_test(void *array, void *copy, size_t cnt, size_t isize, size_t loops)
{
	xtest(xsort, array, copy, cnt, isize, loops);
}

static void
xqsort_test(void *array, void *copy, size_t cnt, size_t isize, size_t loops)
{
	xtest(xqsort, array, copy, cnt, isize, loops);
}


static void
qsort_test(void *array, void *copy, size_t cnt, size_t isize, size_t loops)
{
	xtest(qsort, array, copy, cnt, isize, loops);
}

static void
smsort_test(void *array, void *copy, size_t cnt, size_t isize, size_t loops)
{
	xtest(smsort, array, copy, cnt, isize, loops);
}

static void
smsorte_test(void *array, void *copy, size_t cnt, size_t isize, size_t loops)
{
	smsort_less_t less = get_less_routine(isize);
	smsort_swap_t swap = get_swap_routine(isize);
	size_t len = cnt * isize;

	do {
		memcpy(copy, array, len);
		smsort_ext(copy, 0, cnt, less, swap);
	} while (--loops > 0);
}

static void
dump_unsorted(const void *copy, size_t cnt, size_t isize, size_t failed)
{
	size_t i;

	printf("unsorted array (at index %lu):\n", (ulong) failed);

	for (i = 0; i < cnt; i++) {
		char buf[DUMP_BYTES * 2 + 1];
		size_t n;
		const char *cur = const_ptr_add_offset(copy, i * isize);

		n = base16_encode(buf, sizeof buf - 1, cur, MIN(isize, DUMP_BYTES));
		buf[n] = '\0';
		printf("%6lu %s%s%s\n", (ulong) i, buf,
			isize > DUMP_BYTES ? "..." : "",
			i == failed ? " <-- FAILED" : "");
	}
	abort();
}

static void
assert_is_sorted(const void *copy, size_t cnt, size_t isize)
{
	cmp_routine cmp = get_cmp_routine(isize);
	size_t i;

	for (i = 1; i < cnt; i++) {
		const char *prev = const_ptr_add_offset(copy, (i - 1) * isize);
		const char *cur = const_ptr_add_offset(copy, i * isize);

		if ((*cmp)(prev, cur) > 0)
			dump_unsorted(copy, cnt, isize, i);
	}
}

static double
dry_run(void *array, void *copy, size_t cnt, size_t isize, size_t loops)
{
	tm_t start, end;
	double ustart, uend;

	tm_now_exact(&start);
	tm_cputime(&ustart, NULL);
	qsort_test(array, copy, cnt, isize, loops);
	tm_cputime(&uend, NULL);
	tm_now_exact(&end);

	return ustart == uend ? tm_elapsed_f(&end, &start) : uend - ustart;
}

static size_t
calibrate(void *array, size_t cnt, size_t isize)
{
	double elapsed;
	size_t n = 1;
	void *copy;

	copy = xmalloc(cnt * isize);

	do {
		n *= 2;
		elapsed = dry_run(array, copy, cnt, isize, n);
	} while (elapsed < 0.1 && n < (1U << 31));

	xfree(copy);

	return n;
}

static void
timeit(void (*f)(void *, void *, size_t, size_t, size_t),
	size_t loops, void *array, size_t cnt, size_t isize,
	bool chrono, const char *what, const char *algorithm)
{
	tm_t start, end;
	double ustart, uend;
	void *copy;

	copy = xmalloc(cnt * isize);

	tm_now_exact(&start);
	tm_cputime(&ustart, NULL);
	(*f)(array, copy, cnt, isize, loops);
	tm_cputime(&uend, NULL);
	tm_now_exact(&end);
	assert_is_sorted(copy, cnt, isize);
	xfree(copy);

	if (chrono) {
		double elapsed = tm_elapsed_f(&end, &start);
		double cpu = uend - ustart;
		printf("%7s - %s - [%lu] time=%.3gs, CPU=%.3gs\n", algorithm, what,
			(ulong) loops, elapsed, cpu);
	} else {
		printf("%7s - %s - OK\n", algorithm, what);
	}
	fflush(stdout);
}

static void *
generate_array(size_t cnt, size_t isize)
{
	size_t len;
	void *array;
	
	len = cnt * isize;
	array = xmalloc(len);
	rand31_bytes(array, len);

	return array;
}

static void
perturb_sorted_array(void *array, size_t cnt, size_t isize)
{
	size_t n;
	size_t i;
	void *tmp;

	xsort(array, cnt, isize, get_cmp_routine(isize));

	n = 1 + rand31_value(cnt / 16);
	tmp = alloca(isize);

	for (i = 0; i < n; i++) {
		size_t a = rand31_value(cnt - 1);
		size_t b = rand31_value(cnt - 1);
		void *x = ptr_add_offset(array, a * isize);
		void *y = ptr_add_offset(array, b * isize);

		memcpy(tmp, y, isize);
		memcpy(y, x, isize);
		memcpy(x, tmp, isize);
	}
}

static void
run(void *array, size_t cnt, size_t isize, bool chrono, size_t loops,
	const char *what)
{
	if (0 == loops)
		loops = chrono ? calibrate(array, cnt, isize) : 1;

	timeit(xsort_test, loops, array, cnt, isize, chrono, what, "xsort");
	timeit(xqsort_test, loops, array, cnt, isize, chrono, what, "xqsort");
	timeit(qsort_test, loops, array, cnt, isize, chrono, what, "qsort");
	timeit(smsort_test, loops, array, cnt, isize, chrono, what, "smooth");
	timeit(smsorte_test, loops, array, cnt, isize, chrono, what, "smoothe");
}

static void
test(size_t cnt, size_t isize, bool chrono, size_t loops)
{
	char buf[80];
	void *array;
	void *copy;

	str_bprintf(buf, sizeof buf, "%zu item%s of %zu bytes",
		cnt, 1 == cnt ? "" : "s", isize);

	array = generate_array(cnt, isize);
	copy = xcopy(array, cnt * isize);

	run(array, cnt, isize, chrono, loops, buf);

	str_bprintf(buf, sizeof buf, "%zu sorted item%s of %zu bytes",
		cnt, 1 == cnt ? "" : "s", isize);

	xsort(array, cnt, isize, get_cmp_routine(isize));
	run(array, cnt, isize, chrono, loops, buf);

	str_bprintf(buf, sizeof buf,
		"%zu almost sorted item%s of %zu bytes",
		cnt, 1 == cnt ? "" : "s", isize);

	perturb_sorted_array(array, cnt, isize);
	run(array, cnt, isize, chrono, loops, buf);

	str_bprintf(buf, sizeof buf,
		"%zu reverse-sorted item%s of %zu bytes",
		cnt, 1 == cnt ? "" : "s", isize);

	xsort(array, cnt, isize, get_revcmp_routine(isize));
	run(array, cnt, isize, chrono, loops, buf);

	str_bprintf(buf, sizeof buf,
		"%zu almost rev-sorted item%s of %zu bytes",
		cnt, 1 == cnt ? "" : "s", isize);

	perturb_sorted_array(array, cnt, isize);
	run(array, cnt, isize, chrono, loops, buf);

	str_bprintf(buf, sizeof buf,
		"%zu sorted 3/4-1/4 item%s of %zu bytes",
		cnt, 1 == cnt ? "" : "s", isize);

	memcpy(array, copy, cnt * isize);

	{
		size_t thresh = cnt / 4;
		size_t lower = cnt - thresh;
		void *upper = ptr_add_offset(array, lower * isize);

		xsort(array, lower, isize, get_cmp_routine(isize));
		if (thresh > 0)
			xsort(upper, thresh, isize, get_cmp_routine(isize));
	}
	run(array, cnt, isize, chrono, loops, buf);

	str_bprintf(buf, sizeof buf,
		"%zu sorted n-8 item%s of %zu bytes",
		cnt, 1 == cnt ? "" : "s", isize);

	memcpy(array, copy, cnt * isize);

	{
		size_t thresh = 8;
		size_t lower = cnt - thresh;
		void *upper = ptr_add_offset(array, lower * isize);

		if (cnt > thresh) {
			xsort(array, lower, isize, get_cmp_routine(isize));
			xsort(upper, thresh, isize, get_cmp_routine(isize));
		} else {
			xsort(array, cnt, isize, get_cmp_routine(isize));
		}
	}
	run(array, cnt, isize, chrono, loops, buf);

	xfree(array);
	xfree(copy);
}

int
main(int argc, char **argv)
{
	extern int optind;
	extern char *optarg;
	bool tflag = 0;
	size_t count = 0;
	size_t isize = 0;
	size_t loops = 0;
	int c;
	size_t i;
	unsigned rseed = 0;

	mingw_early_init();
	progname = argv[0];

	while ((c = getopt(argc, argv, "c:hn:s:tR:")) != EOF) {
		switch (c) {
		case 'c':			/* amount of items to use in array */
			count = atol(optarg);
			break;
		case 't':			/* timing report */
			tflag++;
			break;
		case 'n':			/* amount of loops */
			loops = atol(optarg);
			break;
		case 's':			/* item size */
			isize = atol(optarg);
			break;
		case 'R':			/* randomize in a repeatable way */
			rseed = atoi(optarg);
			break;
		case 'h':			/* show help */
		default:
			usage();
			break;
		}
	}

	if ((argc -= optind) != 0)
		usage();

	rand31_set_seed(rseed);

	for (i = 1; i <= TEST_BITS; i++) {
		bool is_last = count != 0;
		size_t cnt = count != 0 ? count : 1U << i;
		size_t j;

		for (j = 0; j < TEST_WORDS; j++) {
			bool is_last_size = isize != 0;
			size_t size = isize != 0 ? isize : sizeof(void *) + INTSIZE * j;

			test(cnt, size, tflag, loops);

			if (is_last_size)
				break;
		}

		if (is_last)
			break;
	}

	return 0;
}

