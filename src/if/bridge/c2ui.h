/*
 * $Id$
 *
 * Copyright (c) 2004, Emile Roberts
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
 * Interface core -> UI.
 *
 * @author Emile Roberts
 * @date 2004
 */

#ifndef _if_bridge_c2ui_h_
#define _if_bridge_c2ui_h_

#include "common.h"

/*
 *	SECTION 1 - Interface includes
 */

#include "if/core/downloads.h"
#include "if/core/uploads.h"
#include "if/core/bitzi.h"

#include "lib/host_addr.h"
#include "lib/misc.h"

/* Property table includes */
#include "if/gui_property.h"

/* download interface functions */
void gcu_download_gui_updates_thaw(void);
void gcu_download_gui_updates_freeze(void);

/* misc interface functions */
void guc_allow_rescan_dir(gboolean flag);
void gcu_gui_update_files_scanned(void);
gint gcu_gtk_main_flush(void);

/** search interface functions */
gboolean gcu_search_gui_new_search(const gchar *query, flag_t flags);

/* statusbar interface functions */
void gcu_statusbar_warning(const gchar *message);
void gcu_statusbar_message(const gchar *message);

/* upload interface functions */
void gcu_upload_stats_gui_add(const struct ul_stats *);
void gcu_upload_stats_gui_update(const struct ul_stats *);
void gcu_upload_stats_gui_clear_all(void);
void gcu_upload_stats_gui_freeze(void);
void gcu_upload_stats_gui_thaw(void);

/** bitzi results */
void gcu_bitzi_result(bitzi_data_t *);

#endif /* _if_bridge_c2ui_h_ */

