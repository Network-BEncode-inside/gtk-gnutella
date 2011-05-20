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
 * GGEP type-specific routines.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#ifndef _core_ggep_type_h_
#define _core_ggep_type_h_

#include "common.h"
#include "lib/host_addr.h"
#include "if/core/search.h"

/**
 * Extraction interface return types.
 */

typedef enum ggept_status {
	GGEP_OK = 0,					/**< OK, extracted what was asked */
	GGEP_NOT_FOUND = 1,				/**< OK, but did not find it */
	GGEP_INVALID = 2,				/**< Error, found something invalid */
	GGEP_BAD_SIZE = 3				/**< Error, buffer not correctly sized */
} ggept_status_t;

/*
 * Public interface.
 */

struct gnutella_host;

ggept_status_t ggept_h_sha1_extract(const extvec_t *, struct sha1 *);
ggept_status_t ggept_h_tth_extract(const extvec_t *, struct tth *);

/** Decompiled payload of "GTKGV1" */
struct ggep_gtkgv1 {
	guint8 major;
	guint8 minor;
	guint8 patch;
	guint8 revchar;
	guint32 release;
	guint32 build;
};

ggept_status_t ggept_gtkgv1_extract(const extvec_t *, struct ggep_gtkgv1 *info);
ggept_status_t ggept_hname_extract(const extvec_t *, char *buf, int len);
ggept_status_t ggept_filesize_extract(const extvec_t *, guint64 *fs);
ggept_status_t ggept_uint32_extract(const extvec_t *exv, guint32 *val);
ggept_status_t ggept_du_extract(const extvec_t *, guint32 *uptime);
ggept_status_t ggept_ct_extract(const extvec_t *, time_t *stamp_ptr);
ggept_status_t ggept_gtkg_ipv6_extract(const extvec_t *, host_addr_t *addr);

ggept_status_t ggept_alt_extract(const extvec_t *, gnet_host_vec_t **hvec);
ggept_status_t ggept_push_extract(const extvec_t *, gnet_host_vec_t **hvec);
ggept_status_t ggept_utf8_string_extract(const extvec_t *, char *b, size_t l);

guint ggept_filesize_encode(guint64 filesize, char *data);
guint ggept_du_encode(guint32 uptime, char *data);
guint ggept_ct_encode(time_t stamp, char *data);
guint ggept_m_encode(guint32 mtype, char *data);

#endif	/* _core_ggep_type_h_ */

/* vi: set ts=4 sw=4 cindent: */
