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

/**
 * @ingroup gtk
 * @file
 *
 * GUI filtering functions.
 *
 * @author Raphael Manfredi
 * @author Richard Eckart
 * @date 2001-2003
 */

#include "gui.h"

RCSID("$Id$");

#include "filter.h"
#include "filter_cb.h"
#include "gtk-missing.h"

#include "gtk/misc.h"

#ifdef USE_GTK1
#include "gtk/gtk1/interface-glade.h"
#endif
#ifdef USE_GTK2
#include "gtk/gtk2/interface-glade.h"
#endif

#include "if/gui_property.h"
#include "if/gui_property_priv.h"

#include "lib/glib-missing.h"
#include "lib/utf8.h"
#include "lib/override.h"		/* Must be the last header included */

#define DEFAULT_TARGET (filter_get_drop_target())

/*
 * Public variables
 */
GtkWidget *filter_dialog = NULL;
GtkWidget *popup_filter_rule = NULL;

/*
 * Private variables
 */
static const gchar * const rule_text_type_labels[] = {
    N_("starts with"),
    N_("contains the words"),
    N_("ends with"),
    N_("contains the substring"),
    N_("matches the regex pattern"),
    N_("is exactly")
};

enum {
	FILTER_NODE_GLOBAL = 0,
	FILTER_NODE_BOUND,
	FILTER_NODE_FREE,
	FILTER_NODE_BUILTIN,

	NUM_FILTER_NODES
};

#ifdef USE_GTK1
static GtkCTreeNode *fl_nodes[NUM_FILTER_NODES];
#endif /* USE_GTK1 */

#ifdef USE_GTK2
static GtkTreeIter *fl_nodes[NUM_FILTER_NODES];
#endif /* USE_GTK2 */

/*
 * Private functions prototypes
 */
static rule_t *filter_gui_get_text_rule(void);
static rule_t *filter_gui_get_ip_rule(void);
static rule_t *filter_gui_get_size_rule(void);
static rule_t *filter_gui_get_jump_rule(void);
static rule_t *filter_gui_get_flag_rule(void);
static rule_t *filter_gui_get_state_rule(void);

#ifdef USE_GTK2
static GtkTreeViewColumn *
add_column(GtkTreeView *tv, const gchar *name, gint id)
{
    GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;
	
	renderer = gtk_cell_renderer_text_new();
	g_object_set(G_OBJECT(renderer),
		"mode",		GTK_CELL_RENDERER_MODE_INERT,
		"xalign",	0.0,
		"ypad",		(guint) GUI_CELL_RENDERER_YPAD,
		(void *) 0);
	column = gtk_tree_view_column_new_with_attributes(name, renderer,
				"text", id,
				(void *) 0);

	g_object_set(G_OBJECT(column),
		"fixed-width", 100,
		"min-width", 1,
		"reorderable", FALSE,
		"resizable", TRUE,
		"sizing", GTK_TREE_VIEW_COLUMN_FIXED,
		(void *) 0);
	
    gtk_tree_view_append_column(tv, column);

	return column;
}

static GtkTreeModel *
create_filters_model(void)
{
	GtkTreeStore *store;
	store = gtk_tree_store_new(6,
				G_TYPE_POINTER,	/* filter_t * */
				G_TYPE_STRING,	/* Filter */
				G_TYPE_STRING,	/* Rules */
				G_TYPE_STRING,	/* Match */
				GDK_TYPE_COLOR,	/* foreground */
				GDK_TYPE_COLOR	/* background */
			);
	return GTK_TREE_MODEL(store);
}

static GtkTreeModel *
create_rules_model(void)
{
	GtkListStore *store;
	store = gtk_list_store_new(5,
				G_TYPE_POINTER,	/* rule_t * */
				G_TYPE_STRING,	/* X */
				G_TYPE_STRING,	/* Condition */
				G_TYPE_STRING,	/* Target */
				G_TYPE_STRING	/* Match */
			);
	return GTK_TREE_MODEL(store);
}

#endif /* USE_GTK2 */

/**
 * Fetch the proper root node for a given filter in the filter tree.
 */
#ifdef USE_GTK1
static GtkCTreeNode *
#endif /* USE_GTK1 */
#ifdef USE_GTK2
static GtkTreeIter *
#endif /* USE_GTK2 */
getFilterRoot(filter_t *f)
{
    if (filter_is_global(f)) {
        return fl_nodes[FILTER_NODE_GLOBAL];
    } else if (filter_is_bound(f)) {
        return fl_nodes[FILTER_NODE_BOUND];
    } else if (filter_is_builtin(f)) {
        return fl_nodes[FILTER_NODE_BUILTIN];
    } else {
        return fl_nodes[FILTER_NODE_FREE];
    }
}

/**
 * Initialize the contents of the dialog editor and some
 * internal variables like the roots in the filter list etc.
 */
void
filter_gui_init(void)
{
	static const struct {
		const gchar *name;
		const guint id;
	} radio_buttons[] = {
#define D(x) "radiobutton_filter_" x 

		{ D("flag_stable_set"),		RULE_FLAG_SET },
		{ D("flag_stable_unset"),	RULE_FLAG_UNSET },
		{ D("flag_stable_ignore"),	RULE_FLAG_IGNORE },

		{ D("flag_busy_set"),		RULE_FLAG_SET },
		{ D("flag_busy_unset"),		RULE_FLAG_UNSET },
		{ D("flag_busy_ignore"),	RULE_FLAG_IGNORE },

		{ D("flag_push_set"),		RULE_FLAG_SET },
		{ D("flag_push_unset"),		RULE_FLAG_UNSET },
		{ D("flag_push_ignore"),	RULE_FLAG_IGNORE },

		/*
		 * The user_data set here is later relevant for
		 * filter_gui_get_state_rule().
		 */
		{ D("state_display_do"),		FILTER_PROP_STATE_DO },
		{ D("state_display_dont"),		FILTER_PROP_STATE_DONT },
		{ D("state_display_ignore"),	FILTER_PROP_STATE_IGNORE },
		{ D("state_display_undef"), 	FILTER_PROP_STATE_UNKNOWN },

		{ D("state_download_do"),		FILTER_PROP_STATE_DO },
		{ D("state_download_dont"), 	FILTER_PROP_STATE_DONT },
		{ D("state_download_ignore"),	FILTER_PROP_STATE_IGNORE },
		{ D("state_download_undef"), 	FILTER_PROP_STATE_UNKNOWN },
#undef D
	};
	static const struct {
		const guint id;
	} menu_items[] = {
		{ RULE_TEXT_PREFIX },
		{ RULE_TEXT_WORDS },
		{ RULE_TEXT_SUFFIX },
		{ RULE_TEXT_SUBSTR },
		{ RULE_TEXT_REGEXP },
		{ RULE_TEXT_EXACT },
	};
    GtkMenu *m;
    guint i;

    if (filter_dialog == NULL)
        return;

#ifdef USE_GTK1
	{
		GtkCList *clist_filter_rules;
		GtkCTree *ctree_filter_filters;

		clist_filter_rules = GTK_CLIST
			(lookup_widget(filter_dialog, "clist_filter_rules"));
		ctree_filter_filters = GTK_CTREE
			(lookup_widget(filter_dialog, "ctree_filter_filters"));

		gtk_clist_set_reorderable(clist_filter_rules, TRUE);
		for (i = 0; i < 4; i++)
			gtk_clist_set_column_width(GTK_CLIST(clist_filter_rules), i,
					filter_rules_col_widths[i]);

		for (i = 0; i < 3; i++)
			gtk_clist_set_column_width(GTK_CLIST(ctree_filter_filters), i,
					filter_filters_col_widths[i]);
	}
#endif /* USE_GTK1 */
#ifdef USE_GTK2
	{
		GtkTreeView *tv_rules;
		GtkTreeView *tv_filters;
		GtkTreeModel *model;

		tv_rules = GTK_TREE_VIEW(lookup_widget(filter_dialog,
									"treeview_filter_rules"));
		tv_filters = GTK_TREE_VIEW(lookup_widget(filter_dialog,
									"treeview_filter_filters"));

		model = create_filters_model();
		gtk_tree_view_set_model(tv_filters, model);	
		add_column(tv_filters, _("Filter"), 1);
		add_column(tv_filters, _("Rule"), 2);
		add_column(tv_filters, _("Match"), 3);
		gtk_tree_view_set_rules_hint(tv_filters, TRUE);
		g_signal_connect(G_OBJECT(tv_filters), "cursor-changed",
			G_CALLBACK(on_treeview_filter_filters_select_row), NULL);

		model = create_rules_model();
		add_column(tv_rules, _("!"), 1);
		add_column(tv_rules, _("Condition"), 2);
		add_column(tv_rules, _("Target"), 3);
		add_column(tv_rules, _("Match"), 4);
		gtk_tree_view_set_model(tv_rules, model);	
		gtk_tree_view_set_rules_hint(tv_rules, TRUE);
		g_signal_connect(G_OBJECT(tv_rules), "cursor-changed",
			G_CALLBACK(on_treeview_filter_rules_select_row), NULL);
		g_signal_connect(GTK_OBJECT(tv_rules), "button-press-event",
			G_CALLBACK(on_treeview_filter_rules_button_press_event), NULL);

		gtk_tree_view_set_reorderable(tv_rules, TRUE);
#if 0
		for (i = 0; i < 4; i++)
			gtk_clist_set_column_width(GTK_CLIST(clist_filter_rules), i,
					filter_rules_col_widths[i]);

		for (i = 0; i < 3; i++)
			gtk_clist_set_column_width(GTK_CLIST(ctree_filter_filters), i,
					filter_filters_col_widths[i]);
#endif
	}
#endif /* USE_GTK2 */

    gtk_notebook_set_show_tabs(
        GTK_NOTEBOOK(lookup_widget(filter_dialog, "notebook_filter_detail")),
        FALSE);

    m = GTK_MENU(gtk_menu_new());
	for (i = 0; i < G_N_ELEMENTS(menu_items); i++) {
		guint id = menu_items[i].id;
    	menu_new_item_with_data(m, _(rule_text_type_labels[id]),
        	GUINT_TO_POINTER(id));
	}

    gtk_option_menu_set_menu(GTK_OPTION_MENU(
			lookup_widget(filter_dialog, "optionmenu_filter_text_type")),
        GTK_WIDGET(m));

    /*
     * The user_data set here is later relevant for filter_gui_get_flag_rule()
     */

	for (i = 0; i < G_N_ELEMENTS(radio_buttons); i++) {
    	gtk_object_set_user_data(
			GTK_OBJECT(lookup_widget(filter_dialog, radio_buttons[i].name)),
			GUINT_TO_POINTER(radio_buttons[i].id));
	}
}

/**
 * Show the dialog on screen and set position.
 */
void
filter_gui_show_dialog(void)
{
    guint32 coord[4] = { 0, 0, 0, 0 };

    if (filter_dialog == NULL)
        return;

    gui_prop_get_guint32(PROP_FILTER_DLG_COORDS, coord, 0, 4);
	gui_fix_coords(coord);

    if (coord[2] != 0 && coord[3] != 0)
        gtk_window_set_default_size(GTK_WINDOW(filter_dialog),
	    coord[2], coord[3]);

    gtk_paned_set_position(
        GTK_PANED(lookup_widget(filter_dialog, "hpaned_filter_main")),
        filter_main_divider_pos);

    gtk_widget_show(filter_dialog);
    gdk_window_raise(filter_dialog->window);
}


#ifdef USE_GTK1
/**
 * Remove all entries from the filter tree.
 */
void
filter_gui_filter_clear_list(void)
{
	static const struct {
		const gchar *title;
	} nodes[] = {
		{ N_("Builtin targets (not editable)") },
		{ N_("Global filters") },
		{ N_("Search filters") },
		{ N_("Free filters") },
	};
    GdkColor *bg_color;
    GtkCTree *ctree_filter_filters;
	guint i;

    if (filter_dialog == NULL)
        return;

    ctree_filter_filters = GTK_CTREE
        (lookup_widget(filter_dialog, "ctree_filter_filters"));

    bg_color = &(gtk_widget_get_style(GTK_WIDGET(ctree_filter_filters))
        ->bg[GTK_STATE_ACTIVE]);

    if (fl_nodes[FILTER_NODE_GLOBAL])
        gtk_ctree_remove_node(GTK_CTREE(ctree_filter_filters),
			fl_nodes[FILTER_NODE_GLOBAL]);
    if (fl_nodes[FILTER_NODE_BOUND])
        gtk_ctree_remove_node(GTK_CTREE(ctree_filter_filters),
			fl_nodes[FILTER_NODE_BOUND]);
    if (fl_nodes[FILTER_NODE_FREE])
        gtk_ctree_remove_node(GTK_CTREE(ctree_filter_filters),
			fl_nodes[FILTER_NODE_FREE]);
    if (fl_nodes[FILTER_NODE_BUILTIN])
        gtk_ctree_remove_node(GTK_CTREE(ctree_filter_filters),
			fl_nodes[FILTER_NODE_BUILTIN]);


	for (i = 0; i < G_N_ELEMENTS(nodes); i++) {
    	gchar *titles[3];
		
    	titles[0] = _(nodes[i].title);
    	titles[1] = "";
    	titles[2] = "";
    	fl_nodes[i] = gtk_ctree_insert_node(GTK_CTREE(ctree_filter_filters),
			NULL, NULL, titles, 0, NULL, NULL, NULL, NULL, FALSE, TRUE);
    	gtk_ctree_node_set_selectable(GTK_CTREE(ctree_filter_filters),
			fl_nodes[i], FALSE);
    	gtk_ctree_node_set_background(GTK_CTREE(ctree_filter_filters),
        	fl_nodes[i], bg_color);
	}
}
#endif /* USE_GTK1 */

#ifdef USE_GTK2
/**
 * Remove all entries from the filter tree.
 */
void
filter_gui_filter_clear_list(void)
{
	static const struct {
		const gchar *title;
	} nodes[] = {
		{ N_("Builtin targets (not editable)") },
		{ N_("Global filters") },
		{ N_("Search filters") },
		{ N_("Free filters") },
	};
    GdkColor *bg_color;
    GtkTreeView *tv;
	GtkTreeModel *model;
	guint i;

    if (filter_dialog == NULL)
        return;

    tv = GTK_TREE_VIEW(lookup_widget(filter_dialog, "treeview_filter_filters"));
	model = gtk_tree_view_get_model(tv);

    bg_color = &(gtk_widget_get_style(GTK_WIDGET(tv))->bg[GTK_STATE_ACTIVE]);

	for (i = 0; i < G_N_ELEMENTS(nodes); i++) {
		GtkTreeIter iter;

		gtk_tree_store_append(GTK_TREE_STORE(model), &iter, NULL);
		gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
			0, (void *) 0,
			1, _(nodes[i].title),
			2, (void *) 0,
			3, (void *) 0,
			(-1));

		G_FREE_NULL(fl_nodes[i]);
    	fl_nodes[i] = g_memdup(&iter, sizeof iter);
	}
}
#endif /* USE_GTK2 */

#ifdef USE_GTK1
/**
 * Adds a filter to the filter list in the dialog. If the filter has a
 * shadow, shadow->current should be set as ruleset. If ruleset is NULL,
 * default to filter->ruleset.
 */
void
filter_gui_filter_add(filter_t *f, GList *ruleset)
{
    const gchar *titles[3];
    GtkCTreeNode *node;
    GtkCTreeNode *parent;
    guint count;
    GtkCTree *ctree_filter_filters;
	gchar buf[G_N_ELEMENTS(titles)][256];

    g_assert(f != NULL);

    if (filter_dialog == NULL)
        return;

    ctree_filter_filters = GTK_CTREE
        (lookup_widget(filter_dialog, "ctree_filter_filters"));

    if (ruleset == NULL)
        ruleset = f->ruleset;

    titles[0] = lazy_utf8_to_ui_string(f->name);
    gm_snprintf(buf[1], sizeof buf[1], "%d", g_list_length(ruleset));
    titles[1] = buf[1];
    count = f->match_count + f->fail_count;
    if (count != 0) {
        if (filter_is_builtin(f)) {
            gm_snprintf(buf[2], sizeof buf[2], "%d", f->match_count);
        } else {
            gm_snprintf(buf[2], sizeof buf[2], "%d/%d (%d%%)",
                f->match_count, count,
				(gint) (f->match_count * 100.0 / count));
        }
        titles[2] = buf[2];
    } else {
        titles[2] = "...";
    }

    parent = getFilterRoot(f);

    node = gtk_ctree_insert_node(
        GTK_CTREE(ctree_filter_filters), parent, NULL,
		deconstify_gpointer(titles),
		0, NULL, NULL, NULL, NULL, TRUE, TRUE);
    gtk_ctree_node_set_row_data(GTK_CTREE(ctree_filter_filters), node, f);

    if (parent == fl_nodes[FILTER_NODE_BUILTIN])
        gtk_ctree_node_set_selectable(GTK_CTREE(ctree_filter_filters),
			node, FALSE);

}
#endif /* USE_GTK1 */


#ifdef USE_GTK2
/**
 * Adds a filter to the filter list in the dialog. If the filter has a
 * shadow, shadow->current should be set as ruleset. If ruleset is NULL,
 * default to filter->ruleset.
 */
void
filter_gui_filter_add(filter_t *f, GList *ruleset)
{
    GtkTreeIter iter;
    GtkTreeIter *parent;
    GtkTreeView *tv;
	GtkTreeModel *model;
	gchar buf[256];
    guint count;

    g_assert(f != NULL);

    if (filter_dialog == NULL)
        return;

	tv = GTK_TREE_VIEW(lookup_widget(filter_dialog, "treeview_filter_filters"));
	model = gtk_tree_view_get_model(tv);

    if (ruleset == NULL)
        ruleset = f->ruleset;

    count = f->match_count + f->fail_count;
    if (count != 0) {
        if (filter_is_builtin(f)) {
            gm_snprintf(buf, sizeof buf, "%d", f->match_count);
        } else {
            gm_snprintf(buf, sizeof buf, "%d/%d (%d%%)",
                f->match_count, count,
				(gint) (f->match_count * 100.0 / count));
        }
    } else {
		g_strlcpy(buf, "...", sizeof buf);
    }

    parent = getFilterRoot(f);

	gtk_tree_store_append(GTK_TREE_STORE(model), &iter, parent);
	gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
		0, f,
		1, lazy_utf8_to_ui_string(f->name),
		2, uint64_to_string(g_list_length(ruleset)),
		3, buf,
		(-1));
}
#endif /* USE_GTK2 */

#ifdef USE_GTK1
/**
 * Update the rule count of a filter in the filter table.
 */
void
filter_gui_update_rule_count(filter_t *f, GList *ruleset)
{
    GtkCTreeNode *parent;
    GtkCTreeNode *node;
    GtkCTree *ctree_filter_filters;

    g_assert(f != NULL);

    if (filter_dialog == NULL)
        return;

    ctree_filter_filters = GTK_CTREE
        (lookup_widget(filter_dialog, "ctree_filter_filters"));

    parent = getFilterRoot(f);
    node = gtk_ctree_find_by_row_data(ctree_filter_filters, parent, f);

    if (node != NULL) {
		gchar buf[32];
		
        gm_snprintf(buf, sizeof buf, "%d", g_list_length(ruleset));
        gtk_ctree_node_set_text(GTK_CTREE(ctree_filter_filters),
			node, 1, buf);
    }
}
#endif /* USE_GTK1 */


#ifdef USE_GTK2
/**
 * Update the rule count of a filter in the filter table.
 */
void
filter_gui_update_rule_count(filter_t *f, GList *ruleset)
{
    GtkTreeIter iter;
    GtkTreeView *tv;
    GtkTreeModel *model;

    g_assert(f != NULL);

    if (filter_dialog == NULL)
        return;

	tv = GTK_TREE_VIEW(lookup_widget(filter_dialog, "treeview_filter_filters"));
	model = gtk_tree_view_get_model(tv);

	if (tree_find_iter_by_data(model, 0, f, &iter)) {
		gchar buf[32];
		
        gm_snprintf(buf, sizeof buf, "%d", g_list_length(ruleset));
		gtk_tree_store_set(GTK_TREE_STORE(model), &iter, 2, buf, (-1));
	}
}
#endif /* USE_GTK2 */

#ifdef USE_GTK1
/**
 * Removes a filter from the list in the dialog.
 */
void
filter_gui_filter_remove(filter_t *f)
{
    GtkCTreeNode *parent;
    GtkCTreeNode *node;
    GtkCTree *ctree_filter_filters;

    g_assert(f != NULL);

    if (filter_dialog == NULL)
        return;

    ctree_filter_filters = GTK_CTREE
        (lookup_widget(filter_dialog, "ctree_filter_filters"));

    parent = getFilterRoot(f);
    node = gtk_ctree_find_by_row_data(ctree_filter_filters, parent, f);
    if (node != NULL)
        gtk_ctree_remove_node(ctree_filter_filters, node);
}
#endif /* USE_GTK1 */

#ifdef USE_GTK2
/**
 * Removes a filter from the list in the dialog.
 */
void
filter_gui_filter_remove(filter_t *f)
{
    GtkTreeIter iter;
    GtkTreeView *tv;
    GtkTreeModel *model;

    g_assert(f != NULL);

    if (filter_dialog == NULL)
        return;

   	tv = GTK_TREE_VIEW(lookup_widget(filter_dialog, "treeview_filter_filters"));
	model = gtk_tree_view_get_model(tv);

	if (tree_find_iter_by_data(model, 0, f, &iter))
        gtk_tree_store_remove(GTK_TREE_STORE(model), &iter);
}
#endif /* USE_GTK2 */



/**
 * Don't use this directly. Better use filter_set from filter.c.
 * Tell the gui to set itself up to work on the given filter.
 * The information about removeable/active state and ruleset are not
 * taken from the filter!
 *
 * @note
 * This does not rebuild the target combos.
 */
void
filter_gui_filter_set(filter_t *f, gboolean removable,
	gboolean active, GList *ruleset)
{
    static const gchar * const widgets[] = {
		"checkbutton_filter_enabled",
		"button_filter_reset",
		"button_filter_add_rule_text",
		"button_filter_add_rule_ip",
		"button_filter_add_rule_size",
		"button_filter_add_rule_jump",
		"button_filter_add_rule_flag",
		"button_filter_add_rule_state",
#ifdef USE_GTK1
		"clist_filter_rules",
#endif /* USE_GTK1 */
		"entry_filter_name",
    };
#ifdef USE_GTK1
    GtkCTree *ctree;
#endif /* USE_GTK1 */
#ifdef USE_GTK2
	GtkTreeView *tv;
#endif /* USE_GTK2 */

    if (filter_dialog == NULL)
        return;

#ifdef USE_GTK1
    ctree = GTK_CTREE(lookup_widget(filter_dialog, "ctree_filter_filters"));
#endif /* USE_GTK1 */
#ifdef USE_GTK2
    tv = GTK_TREE_VIEW(lookup_widget(filter_dialog, "treeview_filter_filters"));

	gtk_list_store_clear(GTK_LIST_STORE(gtk_tree_view_get_model(
		GTK_TREE_VIEW(lookup_widget(filter_dialog, "treeview_filter_rules")))));
#endif /* USE_GTK2 */

    filter_gui_edit_rule(NULL);

    work_filter = f;

    if (f != NULL) {
        gtk_mass_widget_set_sensitive(filter_dialog,
			widgets, G_N_ELEMENTS(widgets), TRUE);

        gtk_widget_set_sensitive
            (lookup_widget(filter_dialog, "button_filter_remove"), removable);
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON
                (lookup_widget(filter_dialog, "checkbutton_filter_enabled")),
            active);
        gtk_entry_set_text(
            GTK_ENTRY(lookup_widget(filter_dialog, "entry_filter_name")),
            lazy_utf8_to_ui_string(f->name));

        filter_gui_filter_set_enabled(f, active);

        if (gui_debug >= 5)
            printf("showing ruleset for filter: %s\n", f->name);
        filter_gui_set_ruleset(ruleset);

#ifdef USE_GTK1
		{
			GtkCTreeNode *node;

			node = gtk_ctree_find_by_row_data(GTK_CTREE(ctree),
						getFilterRoot(f), f);
			if (node != NULL) {
				gtk_ctree_select(ctree, node);
			} else {
				g_warning("work_filter is not available in filter tree");
				gtk_clist_unselect_all(GTK_CLIST(ctree));
			}
		}
#endif /* USE_GTK1 */
#ifdef USE_GTK2
		{
			GtkTreeIter iter;
			GtkTreeModel *model;

			model = gtk_tree_view_get_model(tv);
			if (tree_find_iter_by_data(model, 0, f, &iter)) {
				GtkTreePath *path, *cursor_path;
				gboolean update;

				path = gtk_tree_model_get_path(model, &iter);
				gtk_tree_view_get_cursor(tv, &cursor_path, NULL);
				
				update = !cursor_path ||
					0 != gtk_tree_path_compare(path, cursor_path);
				
				if (update) {
					GtkTreePath *p;

					p = gtk_tree_path_copy(path);
					while (gtk_tree_path_up(p))
						gtk_tree_view_expand_row(tv, p, FALSE);
					gtk_tree_path_free(p);
					
					gtk_tree_view_set_cursor(tv, path, NULL, FALSE);
				}

				if (cursor_path)
					gtk_tree_path_free(cursor_path);
				gtk_tree_path_free(path);
			} else {
				g_warning("work_filter is not available in filter tree");
        		gtk_tree_selection_unselect_all(
					gtk_tree_view_get_selection(tv));
			}
		}
#endif /* USE_GTK2 */
    } else {
        gtk_entry_set_text(
            GTK_ENTRY(lookup_widget(filter_dialog, "entry_filter_name")), "");
        filter_gui_set_ruleset(NULL);
        filter_gui_filter_set_enabled(NULL, FALSE);

#ifdef USE_GTK1
        gtk_clist_unselect_all(GTK_CLIST(ctree));
#endif /* USE_GTK1 */
#ifdef USE_GTK2
		gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(tv));
#endif /* USE_GTK2 */


        gtk_widget_set_sensitive
            (lookup_widget(filter_dialog, "button_filter_remove"), FALSE);
        gtk_mass_widget_set_sensitive(filter_dialog,
			widgets, G_N_ELEMENTS(widgets), FALSE);
    }
}



/**
 * Tell the gui a given filter is enabled/disabled. If the filter given
 * is NULL, then the widget will be set insensitive and inactive.
 */
void
filter_gui_filter_set_enabled(filter_t *f, gboolean active)
{
    GtkToggleButton *button;
	GdkColor *fg_color, *bg_color;
	GtkWidget *widget;

    if (filter_dialog == NULL)
        return;

    button = GTK_TOGGLE_BUTTON(
				lookup_widget(filter_dialog, "checkbutton_filter_enabled"));

    gtk_widget_set_sensitive(GTK_WIDGET(button), f != NULL);
    if (f == NULL) {
        gtk_toggle_button_set_active(button, FALSE);
        return;
    }
	if (f == work_filter)
		gtk_toggle_button_set_active(button, active);

#ifdef USE_GTK1
	widget = lookup_widget(filter_dialog, "ctree_filter_filters");
#endif /* USE_GTK1 */
#ifdef USE_GTK2
	widget = lookup_widget(filter_dialog, "treeview_filter_filters");
#endif /* USE_GTK2 */
	
	fg_color = &(gtk_widget_get_style(widget)
			->fg[active ? GTK_STATE_NORMAL : GTK_STATE_INSENSITIVE]);
	bg_color = &(gtk_widget_get_style(widget)
			->bg[active ? GTK_STATE_NORMAL : GTK_STATE_INSENSITIVE]);

#ifdef USE_GTK1
	{
		GtkCTreeNode *node;
		GtkCTreeNode *parent;

		parent = getFilterRoot(f);
		node = gtk_ctree_find_by_row_data(GTK_CTREE(widget), parent, f);

		gtk_ctree_node_set_foreground(GTK_CTREE(widget), node, fg_color);
		gtk_ctree_node_set_background(GTK_CTREE(widget), node, bg_color);
	}
#endif /* USE_GTK1 */
#ifdef USE_GTK2
	{
		GtkTreeModel *model;
		GtkTreeIter iter;

		model = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
		if (tree_find_iter_by_data(model, 0, f, &iter)) {
			gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
				4, fg_color,
				5, bg_color,
				(-1));
		}
	}
#endif /* USE_GTK1 */
}


static const gchar *
filter_get_filter_stats(const filter_t *filter)
{
	const gchar *title;
	static gchar buf[256];

	if (filter_is_shadowed(filter)) {
		title = _("new");
	} else {
		guint n;

		n = filter->match_count + filter->fail_count;
		if (n != 0) {
			if (filter_is_builtin(filter)) {
				gm_snprintf(buf, sizeof buf, "%d", filter->match_count);
			} else {
				gm_snprintf(buf, sizeof buf, "%d/%d (%d%%)",
						filter->match_count, n,
						(gint) (filter->match_count * 100.0 / n));
			}
			title = buf;
		} else {
			title = _("none yet");
		}
	}
	return title;
}

#ifdef USE_GTK1
/**
 * Update the filter list with the current stats data from the filters.
 */
void
filter_gui_update_filter_stats(void)
{
    GtkCTree *ctree;
    gint row;

    if (filter_dialog == NULL)
        return;

    ctree = GTK_CTREE(lookup_widget(filter_dialog, "ctree_filter_filters"));
    gtk_clist_freeze(GTK_CLIST(ctree));

    for (row = 0; row < GTK_CLIST(ctree)->rows; row ++) {
		const gchar *title;
        filter_t *filter;
        GtkCTreeNode *node;

        node = gtk_ctree_node_nth(GTK_CTREE(ctree), row);
        filter = gtk_ctree_node_get_row_data(GTK_CTREE(ctree), node);

        if (filter == NULL)
            continue;

		title = filter_get_filter_stats(filter);
        gtk_ctree_node_set_text(GTK_CTREE(ctree), node, 2, title);
    }

    gtk_clist_thaw(GTK_CLIST(ctree));
}
#endif /* USE_GTK1 */

#ifdef USE_GTK2
static gboolean
filter_update_filter_stats_helper(GtkTreeModel *model, GtkTreePath *unused_path,
	GtkTreeIter *iter, gpointer unused_udata)
{
	const gchar *title;
	filter_t *filter;
	gpointer p = NULL;

	(void) unused_path;
	(void) unused_udata;
	
   	gtk_tree_model_get(model, iter, 0, &p, (-1));
	filter = p;

	if (filter == NULL)
		return FALSE;

	title = filter_get_filter_stats(filter);
	gtk_tree_store_set(GTK_TREE_STORE(model), iter, 3, title, (-1));
	return FALSE; /* continue traversal */
}

/**
 * Update the filter list with the current stats data from the filters.
 */
void
filter_gui_update_filter_stats(void)
{
    if (filter_dialog == NULL)
        return;

	gtk_tree_model_foreach(
		gtk_tree_view_get_model(GTK_TREE_VIEW(
			lookup_widget(filter_dialog, "treeview_filter_filters"))),
		filter_update_filter_stats_helper,
		NULL);
}
#endif /* USE_GTK2 */

static const gchar *
filter_get_rule_stats(const rule_t *rule)
{
	const gchar *title;
	static gchar buf[256];

	if (RULE_IS_SHADOWED(rule)) {
		title = _("new");
	} else {
		gint n;

		n = rule->match_count + rule->fail_count;
		if (n != 0) {
			gm_snprintf(buf, sizeof buf, "%d/%d (%d%%)",
					rule->match_count, n,
					(gint) (rule->match_count * 100.0 / n));
			title = buf;
		} else {
			title = _("none yet");
		}
	}
	return title;
}

#ifdef USE_GTK1
/**
 * Update the rules list with the current stats data from the rules.
 */
void
filter_gui_update_rule_stats(void)
{
    GtkCList *clist;
    gint row;

    if (filter_dialog == NULL || work_filter == NULL)
        return;

    clist = GTK_CLIST(lookup_widget(filter_dialog, "clist_filter_rules"));
    gtk_clist_freeze(GTK_CLIST(clist));

    for (row = 0; row < GTK_CLIST(clist)->rows; row++) {
        const gchar *title;
        rule_t *rule;

        rule = gtk_clist_get_row_data(GTK_CLIST(clist), row);
        if (rule == NULL)
            continue;

		title = filter_get_rule_stats(rule);
        gtk_clist_set_text(GTK_CLIST(clist), row, 3, title);
    }

    gtk_clist_thaw(GTK_CLIST(clist));
}
#endif /* USE_GTK1 */

#ifdef USE_GTK2
static gboolean
filter_update_rule_stats_helper(GtkTreeModel *model, GtkTreePath *unused_path,
	GtkTreeIter *iter, gpointer unused_udata)
{
	const gchar *title;
	rule_t *rule;
	gpointer p = NULL;

	(void) unused_path;
	(void) unused_udata;
	
   	gtk_tree_model_get(model, iter, 0, &p, (-1));
	rule = p;
	if (rule == NULL)
		return FALSE; /* continue traversal */

	title = filter_get_rule_stats(rule);
	gtk_list_store_set(GTK_LIST_STORE(model), iter, 4, title, (-1));
	return FALSE; /* continue traversal */
}

/**
 * Update the rules list with the current stats data from the rules.
 */
void
filter_gui_update_rule_stats(void)
{
    if (filter_dialog == NULL || work_filter == NULL)
        return;

	gtk_tree_model_foreach(
		gtk_tree_view_get_model(GTK_TREE_VIEW(
			lookup_widget(filter_dialog, "treeview_filter_rules"))),
		filter_update_rule_stats_helper,
		NULL);
}
#endif /* USE_GTK2 */


void
filter_gui_rebuild_target_combos(GList *filters)
{
    static const struct {
		const gchar *name;
	} opt_menus[] = {
		{ "optionmenu_filter_text_target"  },
		{ "optionmenu_filter_ip_target"    },
		{ "optionmenu_filter_size_target"  },
		{ "optionmenu_filter_jump_target"  },
		{ "optionmenu_filter_sha1_target"  },
		{ "optionmenu_filter_flag_target"  },
		{ "optionmenu_filter_state_target" },
	};
    GtkWidget *optionmenu_search_filter;
    GtkMenu *m;
    GList *l_iter;
    GList *buf = NULL;
    gpointer bufptr;

    /*
     * Prepare a list of unbound filters and also leave
     * out the global and builtin filters.
     */
    for (l_iter = filters; l_iter != NULL; l_iter = g_list_next(l_iter)) {
        filter_t *filter = l_iter->data;

        if (!filter_is_bound(filter) && !filter_is_global(filter))
            buf = g_list_append(buf, filter);
    }

    /*
     * These can only be updated if there is a dialog.
     */
    if (filter_dialog != NULL) {
    	guint i;

        for (i = 0; i < G_N_ELEMENTS(opt_menus); i++) {
            m = GTK_MENU(gtk_menu_new());

            for (l_iter = buf; l_iter != NULL; l_iter = g_list_next(l_iter)) {
                filter_t *filter = l_iter->data;
                if (filter != work_filter) {
					const gchar *s = lazy_utf8_to_ui_string(filter->name);
                    menu_new_item_with_data(m, deconstify_gchar(s), filter);
				}
            }

            gtk_option_menu_set_menu(
               GTK_OPTION_MENU(lookup_widget(filter_dialog, opt_menus[i].name)),
               GTK_WIDGET(m));
        }
    }

    /*
     * The following is in the main window and should always be
     * updateable.
     */
    optionmenu_search_filter = lookup_widget
        (main_window, "optionmenu_search_filter");

    bufptr = option_menu_get_selected_data(
				GTK_OPTION_MENU(optionmenu_search_filter));

    m = GTK_MENU(gtk_menu_new());

    menu_new_item_with_data(m, _("no default filter"), NULL);
    for (l_iter = buf; l_iter != NULL; l_iter = g_list_next(l_iter)) {
        filter_t *filter = l_iter->data;
        /*
         * This is no need to create a query which should not
         * display anything, also we can't advertise a filter
         * as target that does not really exist yet.
         */
        if (!filter_is_builtin(filter) && !filter_is_shadowed(filter)) {
			const gchar *s = lazy_utf8_to_ui_string(filter->name);
            menu_new_item_with_data(m, deconstify_gchar(s), filter);
		}
    }

    gtk_option_menu_set_menu(GTK_OPTION_MENU(optionmenu_search_filter),
		GTK_WIDGET(m));

	if (bufptr)
    	option_menu_select_item_by_data(
			GTK_OPTION_MENU(optionmenu_search_filter), bufptr);

    g_list_free(buf);
}



/**
 * Load the given rule into the detail view.
 */
void
filter_gui_edit_rule(rule_t *r)
{
    if (filter_dialog == NULL)
        return;

    if (r != NULL) {
        switch (r->type) {
        case RULE_TEXT:
            filter_gui_edit_text_rule(r);
            break;
        case RULE_IP:
            filter_gui_edit_ip_rule(r);
            break;
        case RULE_SIZE:
            filter_gui_edit_size_rule(r);
            break;
        case RULE_JUMP:
            filter_gui_edit_jump_rule(r);
            break;
        case RULE_SHA1:
            filter_gui_edit_sha1_rule(r);
            break;
        case RULE_FLAG:
            filter_gui_edit_flag_rule(r);
            break;
        case RULE_STATE:
            filter_gui_edit_state_rule(r);
            break;
        default:
            g_error("Unknown rule type: %d", r->type);
        }
    } else {
        gtk_notebook_set_page(
            GTK_NOTEBOOK
                (lookup_widget(filter_dialog, "notebook_filter_detail")),
            nb_filt_page_buttons);

#ifdef USE_GTK1
		gtk_clist_unselect_all(GTK_CLIST(lookup_widget(filter_dialog,
										"clist_filter_rules")));
#endif /* USE_GTK1 */
#ifdef USE_GTK2
		gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(
				GTK_TREE_VIEW(lookup_widget(filter_dialog,
					"treeview_filter_rules"))));
#endif /* USE_GTK2 */
    }
}



/**
 * Load a ip rule into the rule edtior or clear it if the rule is NULL.
 */
void
filter_gui_edit_ip_rule(rule_t *r)
{
    gchar *ip;
    gpointer target = DEFAULT_TARGET;
    gboolean invert = FALSE;
    gboolean active = TRUE;
    gboolean soft   = FALSE;

    g_assert(r == NULL || r->type == RULE_IP);

    if (filter_dialog == NULL)
        return;

    if (r != NULL) {
        ip   = g_strdup_printf("%s/%u",
					host_addr_to_string(r->u.ip.addr), r->u.ip.mask);
        target = r->target;
        invert = RULE_IS_NEGATED(r);
        active = RULE_IS_ACTIVE(r);
        soft   = RULE_IS_SOFT(r);
    } else {
		ip = g_strdup("");
	}

    gtk_entry_set_text(
        GTK_ENTRY(lookup_widget(filter_dialog, "entry_filter_ip_address")),
        ip);
    option_menu_select_item_by_data(GTK_OPTION_MENU(
			lookup_widget(filter_dialog, "optionmenu_filter_ip_target")),
        target);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(lookup_widget
            (filter_dialog, "checkbutton_filter_ip_invert_cond")),
        invert);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_ip_active")),
        active);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_ip_soft")),
        soft);

   	G_FREE_NULL(ip);

    gtk_notebook_set_page(
        GTK_NOTEBOOK(lookup_widget(filter_dialog, "notebook_filter_detail")),
        nb_filt_page_ip);
}



/**
 * Load a sha1 rule into the rule edtior or clear it if the rule is NULL.
 */
void
filter_gui_edit_sha1_rule(rule_t *r)
{
    gchar *hash     = "";
    gchar *origfile = "";
    gpointer target = DEFAULT_TARGET;
    gboolean invert = FALSE;
    gboolean active = TRUE;
    gboolean soft   = FALSE;

    g_assert(r == NULL || r->type == RULE_SHA1);

    if (filter_dialog == NULL)
        return;

    if (r != NULL) {
        hash = r->u.sha1.hash != NULL ?
            sha1_base32(r->u.sha1.hash) : _("[no hash]");
        origfile = r->u.sha1.filename;
        target = r->target;
        invert = RULE_IS_NEGATED(r);
        active = RULE_IS_ACTIVE(r);
        soft   = RULE_IS_SOFT(r);
    }

    gtk_entry_set_text(
        GTK_ENTRY(lookup_widget(filter_dialog, "entry_filter_sha1_hash")),
        hash);
    gtk_entry_set_text(
        GTK_ENTRY(lookup_widget(filter_dialog, "entry_filter_sha1_origfile")),
        lazy_utf8_to_ui_string(origfile));
    option_menu_select_item_by_data(GTK_OPTION_MENU(
        	lookup_widget(filter_dialog, "optionmenu_filter_sha1_target")),
        target);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(lookup_widget
            (filter_dialog, "checkbutton_filter_sha1_invert_cond")),
        invert);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_sha1_active")),
        active);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_sha1_soft")),
        soft);

    gtk_notebook_set_page(
        GTK_NOTEBOOK
            (lookup_widget(filter_dialog, "notebook_filter_detail")),
        nb_filt_page_sha1);
}



/**
 * Load a text rule into the rule edtior or clear it if the rule is NULL.
 */

void
filter_gui_edit_text_rule(rule_t *r)
{
    gchar *pattern  = "";
    guint type      = RULE_TEXT_WORDS;
    gboolean tcase  = FALSE;
    gpointer target = DEFAULT_TARGET;
    gboolean invert = FALSE;
    gboolean active = TRUE;
    gboolean soft   = FALSE;

    g_assert(r == NULL || r->type == RULE_TEXT);

    if (filter_dialog == NULL)
        return;

    if (r != NULL) {
        pattern = r->u.text.match;
        type    = r->u.text.type;
        tcase   = r->u.text.case_sensitive;
        target  = r->target;
        invert  = RULE_IS_NEGATED(r);
        active  = RULE_IS_ACTIVE(r);
        soft    = RULE_IS_SOFT(r);
    }

    gtk_entry_set_text(
        GTK_ENTRY(lookup_widget(filter_dialog, "entry_filter_text_pattern")),
        lazy_utf8_to_ui_string(pattern));
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_text_case")),
        tcase);
    gtk_option_menu_set_history(GTK_OPTION_MENU(
			lookup_widget(filter_dialog, "optionmenu_filter_text_type")),
        type);
    option_menu_select_item_by_data(GTK_OPTION_MENU(
        	lookup_widget(filter_dialog, "optionmenu_filter_text_target")),
        target);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(lookup_widget
            (filter_dialog, "checkbutton_filter_text_invert_cond")),
        invert);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_text_active")),
        active);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_text_soft")),
        soft);

    gtk_notebook_set_page(
        GTK_NOTEBOOK
            (lookup_widget(filter_dialog, "notebook_filter_detail")),
        nb_filt_page_text);
}



/**
 * Load a size rule into the rule edtior or clear it if the rule is NULL.
 */
void
filter_gui_edit_size_rule(rule_t *r)
{
    filesize_t min     = 0;
    filesize_t max     = 0;
    gpointer target = DEFAULT_TARGET;
    gboolean invert = FALSE;
    gboolean active = TRUE;
    gboolean soft   = FALSE;

    g_assert(r == NULL || r->type == RULE_SIZE);

    if (filter_dialog == NULL)
        return;

    if (r != NULL) {
        min    = r->u.size.lower;
        max    = r->u.size.upper;
        target = r->target;
        invert = RULE_IS_NEGATED(r);
        active = RULE_IS_ACTIVE(r);
        soft   = RULE_IS_SOFT(r);
    }

    gtk_entry_printf(
        GTK_ENTRY(lookup_widget(filter_dialog, "entry_filter_size_min")),
		"%s", uint64_to_string(min));
    gtk_entry_printf(
        GTK_ENTRY(lookup_widget(filter_dialog, "entry_filter_size_max")),
		"%s", uint64_to_string(max));
    option_menu_select_item_by_data(GTK_OPTION_MENU(
        	lookup_widget(filter_dialog, "optionmenu_filter_size_target")),
        target);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(lookup_widget
            (filter_dialog, "checkbutton_filter_size_invert_cond")),
        invert);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_size_active")),
        active);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_size_soft")),
        soft);

    gtk_notebook_set_page(
        GTK_NOTEBOOK
            (lookup_widget(filter_dialog, "notebook_filter_detail")),
        nb_filt_page_size);
}



/**
 * Load a jump rule into the rule edtior or clear it if the rule is NULL.
 */
void
filter_gui_edit_jump_rule(rule_t *r)
{
    gpointer target = DEFAULT_TARGET;
    gboolean active = TRUE;

    g_assert(r == NULL || r->type == RULE_JUMP);

    if (filter_dialog == NULL)
        return;

    if (r != NULL) {
        target = r->target;
        active = RULE_IS_ACTIVE(r);
    }

    option_menu_select_item_by_data(GTK_OPTION_MENU(
        	lookup_widget(filter_dialog, "optionmenu_filter_jump_target")),
        target);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_jump_active")),
        active);

    gtk_notebook_set_page(
        GTK_NOTEBOOK
            (lookup_widget(filter_dialog, "notebook_filter_detail")),
        nb_filt_page_jump);
}



/**
 * Load a flag rule into the rule edtior or clear it if the rule is NULL.
 */
void
filter_gui_edit_flag_rule(rule_t *r)
{
    guint stable    = RULE_FLAG_IGNORE;
    guint busy      = RULE_FLAG_IGNORE;
    guint push      = RULE_FLAG_IGNORE;
    gpointer target = DEFAULT_TARGET;
    gboolean active = TRUE;
    gboolean soft   = FALSE;
    gchar *widget   = NULL;

    g_assert(r == NULL || r->type == RULE_FLAG);

    if (filter_dialog == NULL)
        return;

    if (r != NULL) {
        stable = r->u.flag.stable;
        busy   = r->u.flag.busy;
        push   = r->u.flag.push;
        active = RULE_IS_ACTIVE(r);
        soft   = RULE_IS_SOFT(r);
    }

    switch (stable) {
    case RULE_FLAG_SET:
        widget = "radiobutton_filter_flag_stable_set";
        break;
    case RULE_FLAG_UNSET:
        widget = "radiobutton_filter_flag_stable_unset";
        break;
    case RULE_FLAG_IGNORE:
        widget = "radiobutton_filter_flag_stable_ignore";
        break;
    default:
        g_assert_not_reached();
    }
    gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(lookup_widget(filter_dialog, widget)), TRUE);

    switch (busy) {
    case RULE_FLAG_SET:
        widget = "radiobutton_filter_flag_busy_set";
        break;
    case RULE_FLAG_UNSET:
        widget = "radiobutton_filter_flag_busy_unset";
        break;
    case RULE_FLAG_IGNORE:
        widget = "radiobutton_filter_flag_busy_ignore";
        break;
    default:
        g_assert_not_reached();
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(
			lookup_widget(filter_dialog, widget)), TRUE);

    switch (push) {
    case RULE_FLAG_SET:
        widget = "radiobutton_filter_flag_push_set";
        break;
    case RULE_FLAG_UNSET:
        widget = "radiobutton_filter_flag_push_unset";
        break;
    case RULE_FLAG_IGNORE:
        widget = "radiobutton_filter_flag_push_ignore";
        break;
    default:
        g_assert_not_reached();
    }
    gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(lookup_widget(filter_dialog, widget)), TRUE);

    option_menu_select_item_by_data(
        GTK_OPTION_MENU(lookup_widget(filter_dialog,
				"optionmenu_filter_flag_target")),
        target);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_flag_active")),
        active);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_flag_soft")),
        soft);

    gtk_notebook_set_page(
        GTK_NOTEBOOK
            (lookup_widget(filter_dialog, "notebook_filter_detail")),
        nb_filt_page_flag);
}




/**
 * Load a state rule into the rule edtior or clear it if the rule is NULL.
 */
void
filter_gui_edit_state_rule(rule_t *r)
{
    guint display   = FILTER_PROP_STATE_IGNORE;
    guint download  = FILTER_PROP_STATE_IGNORE;
    gpointer target = DEFAULT_TARGET;
    gboolean active = TRUE;
    gboolean soft   = FALSE;
    gboolean invert = FALSE;
    gchar *widget   = NULL;

    g_assert(r == NULL || r->type == RULE_STATE);

    if (filter_dialog == NULL)
        return;

    if (r != NULL) {
        display  = r->u.state.display;
        download = r->u.state.download;
        target   = r->target;
        invert   = RULE_IS_NEGATED(r);
        active   = RULE_IS_ACTIVE(r);
        soft     = RULE_IS_SOFT(r);
    }

    switch (display) {
    case FILTER_PROP_STATE_UNKNOWN:
         widget = "radiobutton_filter_state_display_undef";
         break;
    case FILTER_PROP_STATE_DO:
         widget = "radiobutton_filter_state_display_do";
         break;
    case FILTER_PROP_STATE_DONT:
         widget = "radiobutton_filter_state_display_dont";
         break;
    case FILTER_PROP_STATE_IGNORE:
         widget = "radiobutton_filter_state_display_ignore";
         break;
    default:
         g_error("filter_gui_edit_state_rule: unknown property: %d", display);
    }
    gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(lookup_widget(filter_dialog, widget)), TRUE);

    switch (download) {
    case FILTER_PROP_STATE_UNKNOWN:
         widget = "radiobutton_filter_state_download_undef";
         break;
    case FILTER_PROP_STATE_DO:
         widget = "radiobutton_filter_state_download_do";
         break;
    case FILTER_PROP_STATE_DONT:
         widget = "radiobutton_filter_state_download_dont";
         break;
    case FILTER_PROP_STATE_IGNORE:
         widget = "radiobutton_filter_state_download_ignore";
         break;
    default:
         g_error("filter_gui_edit_state_rule: unknown property: %d", download);
    }
    gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(lookup_widget(filter_dialog, widget)), TRUE);

    option_menu_select_item_by_data(
        GTK_OPTION_MENU(lookup_widget(filter_dialog,
				"optionmenu_filter_state_target")),
        target);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(lookup_widget
            (filter_dialog, "checkbutton_filter_state_invert_cond")),
        invert);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_state_active")),
        active);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_state_soft")),
        soft);

    gtk_notebook_set_page(
        GTK_NOTEBOOK
            (lookup_widget(filter_dialog, "notebook_filter_detail")),
        nb_filt_page_state);
}



/**
 * Display the given ruleset in the table.
 */
void
filter_gui_set_ruleset(GList *ruleset)
#ifdef USE_GTK1
{
    GList *l_iter;
    gint count = 0;
    GdkColor *color;
    GtkCList *clist_filter_rules;

    if (filter_dialog == NULL)
        return;

    clist_filter_rules = GTK_CLIST
        (lookup_widget(filter_dialog, "clist_filter_rules"));

    gtk_clist_freeze(clist_filter_rules);
    gtk_clist_clear(clist_filter_rules);

    color = &(gtk_widget_get_style(GTK_WIDGET(clist_filter_rules))
                ->bg[GTK_STATE_INSENSITIVE]);

    gtk_widget_set_sensitive(
        GTK_WIDGET
            (lookup_widget(filter_dialog, "button_filter_reset_all_rules")),
        ruleset != NULL);

    for (l_iter = ruleset; l_iter != NULL; l_iter = g_list_next(l_iter)) {
        rule_t *r = l_iter->data;
        const gchar *titles[4];
        gint row;

        g_assert(r != NULL);
        count++;
        titles[0] = RULE_IS_NEGATED(r) ? "X" : "";
        titles[1] = filter_rule_condition_to_string(r);
        titles[2] = lazy_utf8_to_ui_string(r->target->name);
        titles[3] = "...";

        row = gtk_clist_append(clist_filter_rules, deconstify_gpointer(titles));
        if (!RULE_IS_ACTIVE(r))
             gtk_clist_set_foreground(clist_filter_rules, row, color);
        gtk_clist_set_row_data(clist_filter_rules, row, r);
    }
    gtk_clist_thaw(clist_filter_rules);

    gtk_widget_set_sensitive(
        lookup_widget(filter_dialog, "button_filter_clear"),
        count != 0);

    if (gui_debug >= 5)
        g_message("updated %d items\n", count);
}
#endif
#ifdef USE_GTK2
{
    GList *l_iter;
    gint count = 0;
    GdkColor *color;
    GtkTreeView *tv;
	GtkTreeModel *model;

    if (filter_dialog == NULL)
        return;

	tv = GTK_TREE_VIEW(lookup_widget(filter_dialog, "treeview_filter_rules"));
	model = gtk_tree_view_get_model(tv);
	gtk_list_store_clear(GTK_LIST_STORE(model));

    color = &(gtk_widget_get_style(GTK_WIDGET(tv))->bg[GTK_STATE_INSENSITIVE]);

    gtk_widget_set_sensitive(
        GTK_WIDGET
            (lookup_widget(filter_dialog, "button_filter_reset_all_rules")),
        ruleset != NULL);

    for (l_iter = ruleset; l_iter != NULL; l_iter = g_list_next(l_iter)) {
		GtkTreeIter iter;
        rule_t *r = l_iter->data;

        g_assert(r != NULL);
        count++;

		gtk_list_store_append(GTK_LIST_STORE(model), &iter);
		gtk_list_store_set(GTK_LIST_STORE(model), &iter,
			0, r,
			1, RULE_IS_NEGATED(r) ? "X" : (void *) 0,
			2, filter_rule_condition_to_string(r),
			3, lazy_utf8_to_ui_string(r->target->name),
			4, "...",
			(-1));

#if 0
        if (!RULE_IS_ACTIVE(r))
             gtk_clist_set_foreground(clist_filter_rules, row, color);
#endif
    }

    gtk_widget_set_sensitive(
        lookup_widget(filter_dialog, "button_filter_clear"),
        count != 0);

    if (gui_debug >= 5)
        g_message("updated %d items\n", count);
}
#endif /* USE_GTK2 */



/**
 * Fetch the rule which is currently edited.
 *
 * @returns a completely new rule_t item in new memory.
 */
rule_t *
filter_gui_get_rule(void)
{
    gint page;
    rule_t *r;

    g_return_val_if_fail(filter_dialog != NULL, NULL);

    page = gtk_notebook_get_current_page
        (GTK_NOTEBOOK
            (lookup_widget(filter_dialog, "notebook_filter_detail")));

    switch (page) {
    case nb_filt_page_buttons:
        r = NULL;
        break;
    case nb_filt_page_text:
        r = filter_gui_get_text_rule();
        break;
    case nb_filt_page_ip:
        r = filter_gui_get_ip_rule();
        break;
    case nb_filt_page_size:
        r = filter_gui_get_size_rule();
        break;
    case nb_filt_page_jump:
        r = filter_gui_get_jump_rule();
        break;
    case nb_filt_page_flag:
        r = filter_gui_get_flag_rule();
        break;
    case nb_filt_page_state:
        r = filter_gui_get_state_rule();
        break;
    default:
        g_assert_not_reached();
        r = NULL;
    };

    if (r != NULL && gui_debug >= 5)
        printf("got rule: %s\n", filter_rule_to_string(r));

    return r;
}



/**
 * Extract information about a text rule.
 *
 * @warning NEVER CALL DIRECTLY!!! Use filter_gui_get_rule().
 */
static rule_t *
filter_gui_get_text_rule(void)
{
  	rule_t *r;
    gchar *match;
    gint type;
    gboolean case_sensitive;
    filter_t *target;
    gboolean negate;
    gboolean active;
    gboolean soft;
    guint16 flags;

    g_return_val_if_fail(filter_dialog != NULL, NULL);

	type = (enum rule_text_type)
        GPOINTER_TO_UINT(option_menu_get_selected_data
            GTK_OPTION_MENU(
				lookup_widget(filter_dialog, "optionmenu_filter_text_type")));

	match = STRTRACK(gtk_editable_get_chars(
        GTK_EDITABLE
            (lookup_widget(filter_dialog, "entry_filter_text_pattern")),
        0, -1));

	case_sensitive = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_text_case")));

	negate = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget
                (filter_dialog, "checkbutton_filter_text_invert_cond")));

	active = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_text_active")));

   	soft = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_text_soft")));

    target = option_menu_get_selected_data(GTK_OPTION_MENU(
				lookup_widget(filter_dialog, "optionmenu_filter_text_target")));

    flags =
        (negate ? RULE_FLAG_NEGATE : 0) |
        (active ? RULE_FLAG_ACTIVE : 0) |
        (soft   ? RULE_FLAG_SOFT   : 0);

    r = filter_new_text_rule(lazy_ui_string_to_utf8(match),
			type, case_sensitive, target, flags);

    G_FREE_NULL(match);

    return r;
}



/**
 * Extract information about a ip rule.
 *
 * @warning NEVER CALL DIRECTLY!!! Use filter_gui_get_rule().
 */
static rule_t *
filter_gui_get_ip_rule(void)
{
	const gchar *ep;
    gchar *s;
    host_addr_t addr;
    guint32 mask;
    filter_t *target;
    gboolean negate;
    gboolean active;
    gboolean soft;
    guint16 flags;
	gint error;

    g_return_val_if_fail(filter_dialog != NULL, NULL);

	s = STRTRACK(gtk_editable_get_chars(
        	GTK_EDITABLE(lookup_widget(filter_dialog,
								"entry_filter_ip_address")),
        	0, -1));
	addr = string_to_host_addr(s, &ep);
	if (*ep == '/')
		mask = parse_uint32(&ep[1], NULL, 10, &error);
	else
		mask = -1;
	G_FREE_NULL(s);

   	negate = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget
                (filter_dialog, "checkbutton_filter_ip_invert_cond")));

	active = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_ip_active")));

   	soft = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_ip_soft")));

    target = option_menu_get_selected_data(GTK_OPTION_MENU(
				lookup_widget(filter_dialog, "optionmenu_filter_ip_target")));

    flags =
        (negate ? RULE_FLAG_NEGATE : 0) |
        (active ? RULE_FLAG_ACTIVE : 0) |
        (soft   ? RULE_FLAG_SOFT   : 0);

    return filter_new_ip_rule(addr, mask, target, flags);
}

guint64
filter_update_size(GtkEntry *entry)
{
	const gchar *text = gtk_entry_get_text(entry);
	gchar buf[32];
	guint64 size = 0;
	gint error = 0;
    const gchar *endptr, *p;

	p = skip_ascii_blanks(text);
	size = parse_uint64(p, &endptr, 10, &error);
	p = skip_ascii_blanks(endptr);

	if (!error && *p != '\0') {
		static const char *suffixes[] = {
			"KB",
			"KiB",
			"MB",
			"MiB",
			"GB",
			"GiB",
			"TB",
			"TiB",
			"PB",
			"PiB",
			"EB",
			"EiB",
		};
		guint64 m10 = 1, m2 = 1;
		guint i;

		error = EINVAL;

		for (i = 0; i < G_N_ELEMENTS(suffixes); i++) {
			gboolean base2 = 0 != (i & 1);
			const gchar *q;

			if (base2) {
				m2 *= 1024;
			} else {
				m10 *= 1000;
			}
			q = is_strcaseprefix(p, suffixes[i]);
			if (NULL != q) {
				guint64 v, mp = base2 ? m2 : m10;

				v = size * mp;
				if ((size == 0 || v > size) && size == v / mp) {
					size = v;
					error = 0;
					p = q;
				} else {
					error = ERANGE;
				}
				break;
			}
		}

		p = skip_ascii_blanks(p);
		if (!error && *p != '\0')
			error = EINVAL;
	}

	if (error) {
		size = 0;
	}

	uint64_to_string_buf(size, buf, sizeof buf);
	if (0 != strcmp(buf, text)) {
		gtk_entry_set_text(entry, buf);
	}

	return size;
}

/**
 * Extract information about a size rule.
 *
 * @warning NEVER CALL DIRECTLY!!! Use filter_gui_get_rule().
 */
static rule_t *
filter_gui_get_size_rule(void)
{
    filesize_t lower;
    filesize_t upper;
    filter_t *target;
    gboolean negate;
    gboolean active;
    gboolean soft;
    guint16 flags;

    if (filter_dialog == NULL)
        return NULL;

    lower = filter_update_size(
		GTK_ENTRY(lookup_widget(filter_dialog, "entry_filter_size_min")));

    upper = filter_update_size(
		GTK_ENTRY(lookup_widget(filter_dialog, "entry_filter_size_max")));

	negate = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget
                (filter_dialog, "checkbutton_filter_size_invert_cond")));

	active = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_size_active")));

   	soft = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_size_soft")));

    target = option_menu_get_selected_data(GTK_OPTION_MENU(
				lookup_widget(filter_dialog, "optionmenu_filter_size_target")));

    flags =
        (negate ? RULE_FLAG_NEGATE : 0) |
        (active ? RULE_FLAG_ACTIVE : 0) |
        (soft   ? RULE_FLAG_SOFT   : 0);

	if (!upper && lower > 0) {
		/* Special fixup for "minimum size" filters */
		upper = lower - 1;
		lower = 0;
		flags |= RULE_FLAG_NEGATE;
	}

	return filter_new_size_rule(lower, upper, target, flags);
}



/**
 * Extract information about a size rule.
 *
 * @warning NEVER CALL DIRECTLY!!! Use filter_gui_get_rule().
 */
static rule_t *
filter_gui_get_jump_rule(void)
{
    filter_t *target;
    gboolean active;
    guint16 flags;

    if (filter_dialog == NULL)
        return NULL;

	active = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_jump_active")));

    target = option_menu_get_selected_data(GTK_OPTION_MENU(
				lookup_widget(filter_dialog, "optionmenu_filter_jump_target")));

    flags = (active ? RULE_FLAG_ACTIVE : 0);

    return filter_new_jump_rule(target, flags);
}



/**
 * Extract information about a flag rule.
 *
 * @warning NEVER CALL DIRECTLY!!! Use filter_gui_get_rule().
 */
static rule_t *
filter_gui_get_flag_rule(void)
{
    filter_t *target;
    enum rule_flag_action stable = 2;
    enum rule_flag_action busy = 2;
    enum rule_flag_action push = 2;
    gboolean active;
    gboolean soft;
    guint16 flags;
    GtkWidget *act;

    if (filter_dialog == NULL)
        return NULL;

    target = option_menu_get_selected_data(GTK_OPTION_MENU(
				lookup_widget(filter_dialog, "optionmenu_filter_flag_target")));

    act = radiobutton_get_active_in_group
        (GTK_RADIO_BUTTON(lookup_widget
            (filter_dialog, "radiobutton_filter_flag_stable_set")));
    stable = (enum rule_flag_action)
        GPOINTER_TO_UINT(gtk_object_get_user_data(GTK_OBJECT(act)));

    act = radiobutton_get_active_in_group
        (GTK_RADIO_BUTTON(lookup_widget
            (filter_dialog, "radiobutton_filter_flag_busy_set")));
    busy = (enum rule_flag_action)
        GPOINTER_TO_UINT(gtk_object_get_user_data(GTK_OBJECT(act)));

    act = radiobutton_get_active_in_group
        (GTK_RADIO_BUTTON(lookup_widget
            (filter_dialog, "radiobutton_filter_flag_push_set")));
    push = (enum rule_flag_action)
        GPOINTER_TO_UINT(gtk_object_get_user_data(GTK_OBJECT(act)));

    active = gtk_toggle_button_get_active
        (GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_flag_active")));

    soft = gtk_toggle_button_get_active
        (GTK_TOGGLE_BUTTON
            (lookup_widget(filter_dialog, "checkbutton_filter_flag_soft")));

    flags = (active ? RULE_FLAG_ACTIVE : 0) |
            (soft   ? RULE_FLAG_SOFT   : 0);

    return filter_new_flag_rule(stable, busy, push, target, flags);
}



/**
 * Extract information about a state rule.
 *
 * @warning NEVER CALL DIRECTLY!!! Use filter_gui_get_rule().
 */
static rule_t *
filter_gui_get_state_rule(void)
{
    filter_t *target;
    enum filter_prop_state display  = FILTER_PROP_STATE_IGNORE;
    enum filter_prop_state download = FILTER_PROP_STATE_IGNORE;
    gboolean active;
    gboolean soft;
    gboolean negate;
    guint16 flags;
    GtkWidget *act;

    if (filter_dialog == NULL)
        return NULL;

    target = option_menu_get_selected_data(GTK_OPTION_MENU(
			lookup_widget(filter_dialog, "optionmenu_filter_state_target")));

    act = radiobutton_get_active_in_group
        (GTK_RADIO_BUTTON(lookup_widget
            (filter_dialog, "radiobutton_filter_state_display_do")));
    display = (enum filter_prop_state)
        GPOINTER_TO_UINT(gtk_object_get_user_data(GTK_OBJECT(act)));

    act = radiobutton_get_active_in_group
        (GTK_RADIO_BUTTON(lookup_widget
            (filter_dialog, "radiobutton_filter_state_download_do")));
    download = (enum filter_prop_state)
        GPOINTER_TO_UINT(gtk_object_get_user_data(GTK_OBJECT(act)));

    active = gtk_toggle_button_get_active
        (GTK_TOGGLE_BUTTON(lookup_widget
            (filter_dialog, "checkbutton_filter_state_active")));

    soft = gtk_toggle_button_get_active
        (GTK_TOGGLE_BUTTON(lookup_widget
            (filter_dialog, "checkbutton_filter_state_soft")));

    negate = gtk_toggle_button_get_active
        (GTK_TOGGLE_BUTTON(lookup_widget
            (filter_dialog, "checkbutton_filter_state_invert_cond")));

    flags = (active ? RULE_FLAG_ACTIVE : 0) |
            (soft   ? RULE_FLAG_SOFT   : 0) |
            (negate ? RULE_FLAG_NEGATE : 0);

    return filter_new_state_rule(display, download, target, flags);
}

#ifdef USE_GTK1
void
filter_gui_freeze_rules(void)
{
    if (filter_dialog == NULL)
        return;

    gtk_clist_freeze
        (GTK_CLIST(lookup_widget(filter_dialog, "clist_filter_rules")));
}

void
filter_gui_thaw_rules(void)
{
    if (filter_dialog == NULL)
        return;

    gtk_clist_thaw
        (GTK_CLIST(lookup_widget(filter_dialog, "clist_filter_rules")));
}

void
filter_gui_freeze_filters(void)
{
    if (filter_dialog == NULL)
        return;

    gtk_clist_freeze
        (GTK_CLIST(lookup_widget(filter_dialog, "ctree_filter_filters")));
}

void
filter_gui_thaw_filters(void)
{
    if (filter_dialog == NULL)
        return;

    gtk_clist_thaw
        (GTK_CLIST(lookup_widget(filter_dialog, "ctree_filter_filters")));
}
#endif /* USE_GTK1 */

#ifdef USE_GTK2

/**
 * Handles filter dialog UI joining.
 *
 * Creates all dependent "tab" windows and merges them into
 * the rules notebook.
 *
 */
GtkWidget *
filter_gui_create_dlg_filters(void)
{
	GtkWidget *dialog;
    GtkWidget *notebook;
    GtkWidget *tab_window[nb_filt_page_num];
    gint i ;

    /*
     * First create the filter dialog without the tab contents.
     */

    dialog = create_dlg_filters();
    notebook = lookup_widget(dialog, "notebook_filter_detail");

    /*
     * Then create all the tabs in their own window.
     */
	tab_window[nb_filt_page_buttons] = create_dlg_filters_add_tab();
	tab_window[nb_filt_page_text] = create_dlg_filters_text_tab();
	tab_window[nb_filt_page_ip] = create_dlg_filters_ip_tab();
	tab_window[nb_filt_page_size] = create_dlg_filters_size_tab();
	tab_window[nb_filt_page_jump] = create_dlg_filters_jump_tab();
	tab_window[nb_filt_page_sha1] = create_dlg_filters_sha1_tab();
	tab_window[nb_filt_page_flag] = create_dlg_filters_flags_tab();
	tab_window[nb_filt_page_state] = create_dlg_filters_state_tab();

    /*
     * Merge the UI and destroy the source windows.
     */
    for (i = 0; i < nb_filt_page_num; i++) {
        GtkWidget *w = tab_window[i];
        gui_merge_window_as_tab(dialog, notebook, w);
        gtk_object_destroy(GTK_OBJECT(w));
    }

    /*
     * Get rid of the first (dummy) notebook tab.
     * (My glade seems to require a tab to be defined in the notebook
     * as a placeholder, or it creates _two_ unlabeled tabs at runtime).
     */
    gtk_container_remove(GTK_CONTAINER(notebook),
        gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), 0));

	return dialog;
}
#endif	/* USE_GTK2 */

/* vi: set ts=4 sw=4 cindent: */
