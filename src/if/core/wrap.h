/*
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
 * @ingroup ui
 * @file
 *
 * Wraping structures for I/O.
 *
 * @author Christian Biere
 * @date 2004
 */

#ifndef _if_core_wrap_h_
#define _if_core_wrap_h_

#include "sockets.h"		/* For enum socket_buftype */

#include "lib/gnet_host.h"

typedef struct wrap_io {
	void *ctx;
	ssize_t (*write)(struct wrap_io *, const void *, size_t);
	ssize_t (*read)(struct wrap_io *, void *, size_t);
	ssize_t (*writev)(struct wrap_io *, const iovec_t *, int);
	ssize_t (*readv)(struct wrap_io *, iovec_t *, int);
	ssize_t (*sendto)(struct wrap_io *, const gnet_host_t *,
						const void *, size_t);
	int (*flush)(struct wrap_io *);
	int (*fd)(struct wrap_io *);
	unsigned (*bufsize)(struct wrap_io *, enum socket_buftype);
} wrap_io_t;

typedef struct wrap_buf {
	size_t	pos;		/**< Current position in the buffer. */
	size_t	len;		/**< Amount of currently buffered bytes. */
	size_t	size;		/**< The size of the buffer. */
	char	*ptr;		/**< The walloc()ed buffer. */
} wrap_buf_t;

#endif /* _if_core_wrap_h_ */

/* vi: set ts=4 sw=4 cindent: */
