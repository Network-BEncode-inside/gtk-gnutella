/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
 *
 * GUI filtering functions.
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

#include "gtk/gui.h"

RCSID("$Id$");

#include "nodes_cb.h"

#include "gtk/gtkcolumnchooser.h"
#include "gtk/nodes_common.h"
#include "gtk/settings.h"
#include "gtk/gtk-missing.h"

#include "if/gui_property_priv.h"
#include "if/bridge/ui2c.h"

#include "lib/override.h"		/* Must be the last header included */

void
on_clist_nodes_select_row
    (GtkCList *clist, gint row, gint col, GdkEvent *event, gpointer user_data)
{
    on_clist_nodes_unselect_row(clist, row, col, event, user_data);
}

void
on_clist_nodes_unselect_row(GtkCList *clist, gint unused_row,
		gint unused_col, GdkEvent *unused_event, gpointer unused_udata)
{
    gboolean sensitive = clist->selection != NULL;

	(void) unused_row;
	(void) unused_col;
	(void) unused_event;
	(void) unused_udata;
	gtk_widget_set_sensitive
        (lookup_widget(main_window, "button_nodes_remove"), sensitive);
    gtk_widget_set_sensitive
        (lookup_widget(popup_nodes, "popup_nodes_remove"), sensitive);
}

void
on_clist_nodes_resize_column(GtkCList *unused_clist, gint column,
		gint width, gpointer unused_udata)
{
	(void) unused_clist;
	(void) unused_udata;
	g_assert(column >= 0 && column <= (gint) G_N_ELEMENTS(nodes_col_widths));
    *(guint32 *) &nodes_col_widths[column] = width; /* override const */
}

gboolean
on_clist_nodes_button_press_event(GtkWidget *unused_widget,
		GdkEventButton *event, gpointer unused_udata)
{
    gint row;
    gint col;
    GtkCList *clist_nodes = GTK_CLIST
        (lookup_widget(main_window, "clist_nodes"));

	(void) unused_widget;
	(void) unused_udata;

    if (event->button != 3)
		return FALSE;

	gtk_widget_set_sensitive(
		lookup_widget(popup_nodes, "popup_nodes_remove"),
		clist_nodes->selection != NULL);

    if (!gtk_clist_get_selection_info
            (clist_nodes, event->x, event->y, &row, &col))
		return FALSE;

    gtk_menu_popup(
        GTK_MENU(popup_nodes), NULL, NULL, NULL, NULL,
        event->button, event->time);

	return TRUE;
}

static gint
list_direct_equal(gconstpointer p1, gconstpointer p2)
{
    return p1 == p2 ? 0 : 1;
}

static void
remove_selected_nodes(void)
{
    GSList *node_list = NULL;
    GtkCList *clist = GTK_CLIST(lookup_widget(main_window, "clist_nodes"));

    g_assert(clist != NULL);

    node_list = clist_collect_data(clist, TRUE, list_direct_equal);
    guc_node_remove_nodes_by_handle(node_list);
    g_slist_free(node_list);
}

static void
add_node(void)
{
    gchar *addr;
    GtkEditable *editable = GTK_EDITABLE
        (lookup_widget(main_window, "entry_host"));

    addr = STRTRACK(gtk_editable_get_chars(editable, 0, -1));
    nodes_gui_common_connect_by_name(addr);
    G_FREE_NULL(addr);
    gtk_entry_set_text(GTK_ENTRY(editable), "");
}

void
on_popup_nodes_remove_activate(GtkMenuItem *unused_menuitem,
		gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;
    remove_selected_nodes();
}

void
on_button_nodes_remove_clicked(GtkButton *unused_button, gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;
    remove_selected_nodes();
}

void
on_button_nodes_add_clicked(GtkButton *unused_button, gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;
    add_node();
}

void
on_entry_host_activate(GtkEditable *unused_editable, gpointer unused_udata)
{
	(void) unused_editable;
	(void) unused_udata;
    add_node();
}

void
on_entry_host_changed(GtkEditable *editable, gpointer unused_udata)
{
	gchar *e;

	(void) unused_udata;
	e = STRTRACK(gtk_editable_get_chars(editable, 0, -1));
	g_strstrip(e);
	gtk_widget_set_sensitive(lookup_widget(main_window, "button_nodes_add"),
        	e[0] != '\0');
	G_FREE_NULL(e);
}

/**
 *  Creates and pops up the column chooser for the ``clist_nodes''.
 */
void
on_popup_nodes_config_cols_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
    GtkWidget *cc;

	(void) unused_menuitem;
	(void) unused_udata;

    cc = gtk_column_chooser_new(lookup_widget(main_window, "clist_nodes"));
    gtk_menu_popup(GTK_MENU(cc), NULL, NULL, NULL, NULL, 1, 0);

    /* GtkColumnChooser takes care of cleaning up itself */
}

/**
 * Initiates a browse host request to the currently selected host.
 */
void
on_popup_nodes_browse_host_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

	/* FIXME: Implement this */	
}

/* vi: set ts=4 sw=4 cindent: */
