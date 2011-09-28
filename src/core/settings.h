/*
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
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
 * gtk-gnutella configuration.
 *
 * @author Raphael Manfredi
 * @author Richard Eckart
 * @date 2001-2003
 */

#ifndef _core_settings_h_
#define _core_settings_h_

#include "common.h"
#include "hcache.h"		/* For host_net_t */

#include "if/core/settings.h"
#include "lib/host_addr.h"

/**
 * Global Data.
 */

extern struct in_addr *local_netmasks;

/*
 * Global Functions
 */

gboolean is_my_address(const host_addr_t addr);
gboolean is_my_address_and_port(const host_addr_t addr, guint16 port);

void settings_early_init(void);
gboolean settings_is_unique_instance(void);
void settings_init(void);
void settings_save_if_dirty(void);
void settings_shutdown(void);
void settings_addr_changed(const host_addr_t new_addr, const host_addr_t peer);
guint32 settings_max_msg_size(void);
void settings_add_randomness();
void settings_close(void);

guint32 get_average_servent_uptime(time_t now);
guint32 get_average_ip_lifetime(time_t now, enum net_type net);

gboolean settings_is_leaf(void);
gboolean settings_is_ultra(void);
gboolean settings_use_ipv4(void);
gboolean settings_use_ipv6(void);
gboolean settings_running_ipv4(void);
gboolean settings_running_ipv6(void);
gboolean settings_running_ipv4_and_ipv6(void);
gboolean settings_running_ipv6_only(void);
gboolean settings_running_same_net(const host_addr_t addr);
gboolean settings_can_connect(const host_addr_t addr);

host_addr_t listen_addr_primary(void);
host_addr_t listen_addr_primary_net(host_net_t net);

#endif /* _core_settings_h_ */

/* vi: set ts=4 sw=4 cindent: */
