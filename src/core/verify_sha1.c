/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
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
 * @ingroup core
 * @file
 *
 * Hash verification.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#include "common.h"

RCSID("$Id$")

#include "verify.h"

#include "lib/misc.h"
#include "lib/sha1.h"

#include "lib/override.h"	/* Must be the last header included */

static struct {
	struct verify	*verify;
	SHA1Context		context;
	struct sha1		digest;
} verify_sha1;

static const char *
verify_sha1_name(void)
{
	return "SHA-1";
}

static void
verify_sha1_reset(filesize_t amount)
{
	int ret;

	(void) amount;
	ret = SHA1Reset(&verify_sha1.context);
	g_assert(shaSuccess == ret);
}

static int
verify_sha1_update(const void *data, size_t size)
{
	int ret;

	ret = SHA1Input(&verify_sha1.context, data, size);
	return shaSuccess == ret ? 0 : -1;
}

static int
verify_sha1_final(void)
{
	int ret;

	ret = SHA1Result(&verify_sha1.context, &verify_sha1.digest);
	return shaSuccess == ret ? 0 : -1;
}

static const struct verify_hash verify_hash_sha1 = {
	verify_sha1_name,
	verify_sha1_reset,
	verify_sha1_update,
	verify_sha1_final,
};

int
verify_sha1_enqueue(int high_priority,
	const char *pathname, filesize_t filesize,
	verify_callback callback, void *user_data)
{
	return verify_enqueue(verify_sha1.verify, high_priority,
		pathname, 0, filesize, callback, user_data);
}

const struct sha1 *
verify_sha1_digest(const struct verify *ctx)
{
	g_return_val_if_fail(verify_status(ctx) == VERIFY_DONE, NULL);
	return &verify_sha1.digest;
}

void
verify_sha1_init(void)
{
	static int initialized;

	if (!initialized) {
		initialized = TRUE;

		verify_sha1.verify = verify_new(&verify_hash_sha1);
	}
}

void
verify_sha1_close(void)
{
	verify_free(&verify_sha1.verify);
}

/* vi: set ts=4 sw=4 cindent: */
