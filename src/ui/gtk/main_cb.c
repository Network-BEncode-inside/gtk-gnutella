/*
 * $Id$
 *
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

#include "gui.h"

RCSID("$Id$")

#include "html_view.h"
#include "main.h"
#include "main_cb.h"
#include "misc.h"
#include "notebooks.h"
#include "settings.h"

#include "if/gui_property.h"
#include "if/bridge/ui2c.h"

#include "lib/file.h"
#include "lib/utf8.h"

#include "lib/override.h"	/* Must be the last header included */

/***
 *** Private functions
 ***/

static struct html_view *faq_html_view;

static void
load_faq(void)
{
	static const gchar faq_file[] = "FAQ";
	static file_path_t fp[4];
	GtkWidget *textview;
	const gchar *lang;
	guint i = 0;
	FILE *f;

	html_view_free(&faq_html_view);

	textview = gui_dlg_faq_lookup("textview_faq");
	lang = locale_get_language();

	file_path_set(&fp[i++], make_pathname(PRIVLIB_EXP, lang), faq_file);
	file_path_set(&fp[i++], PRIVLIB_EXP G_DIR_SEPARATOR_S "en", faq_file);
	
#ifndef OFFICIAL_BUILD
	file_path_set(&fp[i++],
		make_pathname(PACKAGE_EXTRA_SOURCE_DIR, lang), faq_file);

	file_path_set(&fp[i++],
		PACKAGE_EXTRA_SOURCE_DIR G_DIR_SEPARATOR_S "en", faq_file);
#endif /* !OFFICIAL_BUILD */

	g_assert(i <= G_N_ELEMENTS(fp));

	f = file_config_open_read_norename("FAQ", fp, i);
	if (f) {
		faq_html_view = html_view_load_file(textview, fileno(f));
		fclose(f);
	} else {
		static const gchar msg[] =
		N_(
			"<html>"
			"<head>"
			"<title>Frequently Asked Questions</title>"
			"</head>"
			"<body>"
			"<p>"
			"The FAQ document could not be loaded. Please read the "
			"<a href=\"http://gtk-gnutella.sourceforge.net/?page=faq\">"
			"FAQ online</a> instead."
			"</p>"
			"</body>"
			"</html>"
		);

		faq_html_view = html_view_load_memory(textview, array_from_string(msg));
	}
}


static void
quit(gboolean force)
{
    gboolean confirm;

    gui_prop_get_boolean_val(PROP_CONFIRM_QUIT, &confirm);
    if (force || !confirm) {
       	guc_gtk_gnutella_exit(0);
	} else {
        gtk_widget_show(gui_dlg_quit());
    	gdk_window_raise(gui_dlg_quit()->window);
	}
}

/***
 *** Main window
 ***/

gboolean
on_main_window_delete_event(GtkWidget *unused_widget, GdkEvent *unused_event,
		gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;

	quit(FALSE);
	return TRUE;
}

void
on_button_quit_clicked(GtkButton *unused_button, gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;

    quit(FALSE);
}

/***
 *** Tray menu
 ***/

void
on_popup_tray_preferences_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

	main_gui_show_prefences();
}

void
on_popup_tray_quit_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

    quit(FALSE);
}

/***
 *** menu bar
 ***/

void
on_menu_about_activate(GtkMenuItem *unused_menuitem, gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

	g_return_if_fail(gui_dlg_about());
    gtk_widget_show(gui_dlg_about());
	g_return_if_fail(gui_dlg_about()->window);
	gdk_window_raise(gui_dlg_about()->window);
}

void
on_menu_faq_activate(GtkMenuItem *unused_menuitem, gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

	g_return_if_fail(gui_dlg_faq());
	load_faq();
    gtk_widget_show(gui_dlg_faq());
	g_return_if_fail(gui_dlg_faq()->window);
	gdk_window_raise(gui_dlg_faq()->window);
}

void
on_menu_prefs_activate(GtkMenuItem *unused_menuitem, gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

	main_gui_show_prefences();
}

void
on_menu_keyboard_shortcuts_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	(void) unused_menuitem;
	(void) unused_udata;

	g_message("on_menu_keyboard_shortcuts_activate(): This is a stub");
}



/***
 *** about dialog
 ***/

void
on_button_about_close_clicked(GtkButton *unused_button, gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;

	g_return_if_fail(gui_dlg_about());
	
    gtk_widget_hide(gui_dlg_about());
}

gboolean
on_dlg_about_delete_event(GtkWidget *unused_widget, GdkEvent *unused_event,
	gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;

	g_return_val_if_fail(gui_dlg_about(), TRUE);

	gtk_widget_hide(gui_dlg_about());
	return TRUE;
}

gboolean
on_dlg_ancient_delete_event(GtkWidget *unused_widget, GdkEvent *unused_event,
	gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;

	ancient_version_dialog_hide();
	return TRUE;
}
/***
 *** FAQ dialog
 ***/
gboolean
on_dlg_faq_delete_event(GtkWidget *unused_widget, GdkEvent *unused_event,
	gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;

	g_return_val_if_fail(gui_dlg_faq(), TRUE);

	html_view_free(&faq_html_view);
	gtk_widget_hide(gui_dlg_faq());
	return TRUE;
}

/***
 *** prefs dialog
 ***/

void
on_button_prefs_close_clicked(GtkButton *unused_button, gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;

	g_return_if_fail(gui_dlg_prefs());
	g_return_if_fail(GTK_WIDGET_REALIZED(gui_dlg_prefs()));
	g_return_if_fail(GTK_WIDGET_VISIBLE(gui_dlg_prefs()));

	gui_save_window(gui_dlg_prefs(), PROP_PREFS_DLG_COORDS);
    gtk_widget_hide(gui_dlg_prefs());
}

gboolean
on_dlg_prefs_delete_event(GtkWidget *unused_widget, GdkEvent *unused_event,
	gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;

	g_return_val_if_fail(gui_dlg_prefs(), TRUE);
	g_return_val_if_fail(GTK_WIDGET_REALIZED(gui_dlg_prefs()), TRUE);
	g_return_val_if_fail(GTK_WIDGET_VISIBLE(gui_dlg_prefs()), TRUE);

	gtk_widget_hide(gui_dlg_prefs());
	return TRUE;
}


/***
 *** Quit dialog
 ***/

void
on_button_really_quit_clicked(GtkButton *unused_button, gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;
	g_return_if_fail(gui_dlg_quit());

    gtk_widget_hide(gui_dlg_quit());
	quit(TRUE);
}

void
on_button_abort_quit_clicked(GtkButton *unused_button, gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;

	g_return_if_fail(gui_dlg_quit());

    gtk_widget_hide(gui_dlg_quit());
}

gboolean
on_dlg_quit_delete_event(GtkWidget *unused_widget, GdkEvent *unused_event,
	gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;

	g_return_val_if_fail(gui_dlg_quit(), TRUE);
    gtk_widget_hide(gui_dlg_quit());
    return TRUE;
}

/* vi: set ts=4 sw=4 cindent: */
