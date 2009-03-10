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
 * Banning control.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#ifndef _core_ban_h_
#define _core_ban_h_

#include "common.h"
#include "lib/host_addr.h"

struct gnutella_socket;

/**
 * @return codes for ban_allow().
 */

typedef enum {
	BAN_OK		= 0,		/**< OK, don't ban and accept the connection */
	BAN_FIRST	= 1,		/**< Initial banning, send polite denial */
	BAN_FORCE	= 2,		/**< Force banning, don't send back anything */
	BAN_MSG		= 3			/**< Ban with explicit message */
} ban_type_t;

void ban_init(void);
void ban_close(void);
ban_type_t ban_allow(const host_addr_t addr);
void ban_record(const host_addr_t addr, const char *msg);
void ban_force(struct gnutella_socket *s);
int ban_delay(const host_addr_t addr);
const char *ban_message(const host_addr_t addr);
gboolean ban_is_banned(const host_addr_t addr);
void ban_max_recompute(void);

const char *ban_vendor(const char *vendor);

#endif	/* _core_ban_h_ */

/* vi: set ts=4 sw=4 cindent: */
