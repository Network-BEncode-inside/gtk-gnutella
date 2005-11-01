/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Richard Eckart
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

#ifndef _gtk_settings_cb_h_
#define _gtk_settings_cb_h_

#include "gui.h"

void on_spinbutton_search_reissue_timeout_changed(GtkEditable *, gpointer);

gboolean on_entry_config_proxy_hostname_focus_out_event(GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_config_proxy_hostname_activate(GtkEditable *editable, gpointer user_data);

gboolean on_entry_config_socks_password_focus_out_event(GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_config_socks_password_activate(GtkEditable *editable, gpointer user_data);

gboolean on_entry_config_socks_username_focus_out_event(GtkWidget *, GdkEventFocus *, gpointer);
void on_entry_config_socks_username_activate(GtkEditable *, gpointer);

gboolean on_entry_config_extensions_focus_out_event(GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_config_extensions_activate(GtkEditable *editable, gpointer user_data);

gboolean on_entry_config_path_focus_out_event(GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_config_path_activate(GtkEditable *editable, gpointer user_data);

gboolean on_entry_config_force_ip_focus_out_event(GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_config_force_ip_activate(GtkEditable *editable, gpointer user_data);
void on_entry_config_force_ip_changed(GtkEditable *editable, gpointer user_data);

gboolean on_entry_server_hostname_focus_out_event(GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_server_hostname_activate(GtkEditable *editable, gpointer user_data);
void on_entry_server_hostname_changed(GtkEditable *editable, gpointer user_data);

gboolean on_entry_dbg_property_pattern_focus_out_event(GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_dbg_property_pattern_activate(GtkEditable *editable, gpointer user_data);
void on_entry_dbg_property_value_activate(GtkEditable *editable, gpointer user_data);
void on_clist_dbg_property_select_row(GtkCList *clist, gint row, gint unused_column,
	GdkEvent *unused_event, gpointer unused_udata);

void on_menu_statusbar_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_toolbar_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_connections_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_downloads_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_uploads_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_bws_in_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_bws_out_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_bws_gin_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_bws_gout_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_bws_glin_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_bws_glout_visible_activate(GtkMenuItem * menuitem, gpointer user_data);
void on_menu_autohide_bws_gleaf_activate(GtkMenuItem * menuitem, gpointer user_data);

void on_popup_search_toggle_tabs_activate (GtkMenuItem *menuitem, gpointer user_data);

#ifdef USE_GTK2
void
on_button_config_remove_dir_clicked(GtkButton *unused_button,
	gpointer unused_udata);
#endif /* USE_GTK2 */

#endif /* _gtk_settings_cb_h_ */
