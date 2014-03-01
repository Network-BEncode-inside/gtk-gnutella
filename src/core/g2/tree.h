/*
 * Copyright (c) 2012, Raphael Manfredi
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
 * G2 packet management.
 *
 * @author Raphael Manfredi
 * @date 2012
 */

#ifndef _core_g2_tree_h_
#define _core_g2_tree_h_

struct g2_tree;
typedef struct g2_tree g2_tree_t;

/*
 * Public interface.
 */

bool g2_tree_is_valid(const struct g2_tree * const t);
const g2_tree_t *g2_tree_lookup(const g2_tree_t *root, const char *path);
const void *g2_tree_name(const g2_tree_t *root);
const g2_tree_t *g2_tree_root(const g2_tree_t *root);
const void *g2_tree_payload(const g2_tree_t *root, const char *path, size_t *l);
void g2_tree_child_foreach(const g2_tree_t *root, data_fn_t cb, void *data);
const g2_tree_t *g2_tree_first_child(const g2_tree_t *root);
const g2_tree_t *g2_tree_next_child(const g2_tree_t *child);
const g2_tree_t *g2_tree_next_twin(const g2_tree_t *child);
g2_tree_t *g2_tree_alloc_empty(const char *name);
g2_tree_t *g2_tree_alloc(const char *name, const void *payload,
	size_t paylen, bool copy);
void g2_tree_set_payload(g2_tree_t *root, const void *payload,
	size_t paylen, bool copy);
void g2_tree_add_child(g2_tree_t *parent, g2_tree_t *child);
void g2_tree_free(g2_tree_t *root);

#endif /* _core_g2_tree_h_ */

