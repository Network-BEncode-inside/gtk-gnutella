/*
 * Copyright (c) 2002-2004, Raphael Manfredi
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
 * @date 2002-2004
 */

#include "common.h"

#include "ggep.h"
#include "ggep_type.h"
#include "hosts.h"				/* For struct gnutella_host */
#include "ipp_cache.h"			/* For tls_cache_lookup() */
#include "qhit.h"				/* For QHIT_F_* flags */

#include "lib/bstr.h"
#include "lib/endian.h"
#include "lib/gnet_host.h"
#include "lib/misc.h"
#include "lib/sequence.h"
#include "lib/unsigned.h"
#include "lib/utf8.h"
#include "lib/vector.h"
#include "lib/walloc.h"

#include "if/gnet_property_priv.h"
#include "if/core/search.h"

#include "lib/override.h"		/* Must be the last header included */

/**
 * Extract the SHA1 hash of the "H" extension into the supplied buffer.
 *
 * @returns extraction status: only when GGEP_OK is returned will we have
 * the SHA1 in 'sha1'.
 */
ggept_status_t
ggept_h_sha1_extract(const extvec_t *exv, struct sha1 *sha1)
{
	const char *payload;
	size_t tlen;

	g_assert(exv->ext_type == EXT_GGEP);
	g_assert(exv->ext_token == EXT_T_GGEP_H);

	/*
	 * Try decoding as a SHA1 hash, which is <type> <sha1_digest>
	 * for a total of 21 bytes.  We also allow BITRPINT hashes, since the
	 * first 20 bytes of the binary bitprint is actually the SHA1.
	 */

	tlen = ext_paylen(exv);
	if (tlen <= 1)
		return GGEP_INVALID;			/* Can't be a valid "H" payload */

	payload = ext_payload(exv);

	if (payload[0] == GGEP_H_SHA1) {
		if (tlen != (SHA1_RAW_SIZE + 1))
			return GGEP_INVALID;			/* Size is not right */
	} else if (payload[0] == GGEP_H_BITPRINT) {
		if (tlen != (BITPRINT_RAW_SIZE + 1))
			return GGEP_INVALID;			/* Size is not right */
	} else
		return GGEP_NOT_FOUND;

	memcpy(sha1->data, &payload[1], SHA1_RAW_SIZE);

	return GGEP_OK;
}

/**
 * Extract the TTH hash of the "H" extension into the supplied buffer.
 *
 * @returns extraction status: only when GGEP_OK is returned will we have
 * the TTH in 'tth'.
 */
ggept_status_t
ggept_h_tth_extract(const extvec_t *exv, struct tth *tth)
{
	const char *payload;
	size_t tlen;

	g_assert(exv->ext_type == EXT_GGEP);
	g_assert(exv->ext_token == EXT_T_GGEP_H);

	tlen = ext_paylen(exv);
	if (tlen <= 1)
		return GGEP_INVALID;			/* Can't be a valid "H" payload */

	payload = ext_payload(exv);
	if (payload[0] != GGEP_H_BITPRINT)
		return GGEP_NOT_FOUND;
	
	if (tlen != (BITPRINT_RAW_SIZE + 1))
		return GGEP_INVALID;			/* Size is not right */

	memcpy(tth->data, &payload[1 + SHA1_RAW_SIZE], TTH_RAW_SIZE);

	return GGEP_OK;
}

/**
 * The known OS names we encode into the GTKGV extension.
 */
static const char *gtkgv_osname[] = {
	"Unknown OS",				/* 0 */
	"UNIX",						/* 1 */
	"BSD",						/* 2 */
	"Linux",					/* 3 */
	"FreeBSD",					/* 4 */
	"NetBSD",					/* 5 */
	"Windows",					/* 6 */
	"Darwin",					/* 7 */
};

/**
 * @return the OS name encoded into a GTKGV extension.
 */
static const char *
ggept_gtkgv_osname(guint8 value)
{
	return value >= G_N_ELEMENTS(gtkgv_osname) ?
		gtkgv_osname[0] : gtkgv_osname[value];
}

/**
 * Given a system name, look how it should be encoded in GTKGV.
 */
static guint8
ggept_gtkgv_osname_encode(const char *sysname)
{
	guint8 result = 0;
	size_t i;

	/*
	 * First some defaults in case we don't get an exact match.
	 */

	if (is_running_on_mingw())
		result = 6;
	else if (strstr(sysname, "BSD"))
		result = 2;
	else
		result = 1;

	/*
	 * Now attempt a case-insensitive match to see whether we have
	 * something more specific to use than the defaults.
	 */

	for (i = 3; i < G_N_ELEMENTS(gtkgv_osname); i++) {
		if (0 == strcasecmp(sysname, gtkgv_osname[i])) {
			result = i;
			break;
		}
	}

	if (GNET_PROPERTY(ggep_debug)) {
		g_debug("GGEP encoded OS name \"%s\" in GTKGV will be %u",
			sysname, result);
	}

	return result;
}

/**
 * @return the value that should be advertised as the OS name.
 */
guint8
ggept_gtkgv_osname_value(void)
{
	static guint8 result = -1;

	/*
	 * Computation only happens once.
	 */

	if (result >= G_N_ELEMENTS(gtkgv_osname)) {
#ifdef HAS_UNAME
		{
			struct utsname un;

			if (-1 != uname(&un)) {
				result = ggept_gtkgv_osname_encode(un.sysname);
			} else {
				g_carp("uname() failed: %s", g_strerror(errno));
			}
		}
#else
		result = 0;
#endif /* HAS_UNAME */
	}

	return result;
}

/**
 * Extract payload information from "GTKGV" into `info'.
 */
ggept_status_t
ggept_gtkgv_extract(const extvec_t *exv, struct ggep_gtkgv *info)
{
	const char *p;
	int tlen;
	ggept_status_t status = GGEP_OK;

	g_assert(exv->ext_type == EXT_GGEP);
	g_assert(exv->ext_token == EXT_T_GGEP_GTKGV);

	tlen = ext_paylen(exv);

	/*
	 * The original payload length was 13 bytes.
	 *
	 * In order to allow backward-compatible extension of the payload, don't
	 * check for a size equal to 13 bytes but for a size of at least 13.
	 *
	 * Further extensions, if any, will simply append new fields to the payload
	 * which will be ignored (not deserialized) by older versions.  Since the
	 * version number is serialized, it will be possible to derive default
	 * values for older versions of the payload.
	 */

	if (tlen < 13)
		return GGEP_INVALID;

	p = ext_payload(exv);

	info->version = p[0];
	info->major = p[1];
	info->minor = p[2];
	info->patch = p[3];
	info->revchar = p[4];
	info->release = peek_be32(&p[5]);
	info->build = peek_be32(&p[9]);

	info->dirty = FALSE;
	info->commit_len = 0;
	ZERO(&info->commit);
	info->osname = NULL;

	if (info->version >= 1) {
		bstr_t *bs;
		guint8 flags;

		bs = bstr_open(p, tlen, GNET_PROPERTY(ggep_debug) ? BSTR_F_ERROR : 0);
		bstr_skip(bs, 13);

		if (bstr_read_u8(bs, &flags)) {
			guint8 aflags = flags;

			/*
			 * Swallow extra flags, if present (for now we expect only 1 byte).
			 */

			while ((aflags & GTKGV_F_CONT) && bstr_read_u8(bs, &aflags))
				/* empty */;


			info->dirty = booleanize(aflags & GTKGV_F_DIRTY);

			/*
			 * Process git commit SHA1, if present.
			 */

			if (aflags & GTKGV_F_GIT) {
				if (
					bstr_read_u8(bs, &info->commit_len) &&
					info->commit_len != 0
				) {
					if (info->commit_len <= 2 * SHA1_RAW_SIZE) {
						guint8 bytes = (info->commit_len + 1) / 2;
						if (!bstr_read(bs, &info->commit, bytes)) {
							status = GGEP_INVALID;
						}
					} else {
						status = GGEP_INVALID;
					}
				}
			}

			/*
			 * Process OS information is present and we got no error so far.
			 */

			if ((aflags & GTKGV_F_OS) && GGEP_OK == status) {
				guint8 value;

				if (bstr_read_u8(bs, &value)) {
					info->osname = ggept_gtkgv_osname(value);
				}
			}
		}

		bstr_free(&bs);
	}

	return status;
}

/**
 * Extract payload information from "GTKGV1" into `info'.
 */
ggept_status_t
ggept_gtkgv1_extract(const extvec_t *exv, struct ggep_gtkgv1 *info)
{
	const char *p;
	int tlen;

	g_assert(exv->ext_type == EXT_GGEP);
	g_assert(exv->ext_token == EXT_T_GGEP_GTKGV1);

	tlen = ext_paylen(exv);

	if (tlen < 12)
		return GGEP_INVALID;

	p = ext_payload(exv);

	info->major = p[0];
	info->minor = p[1];
	info->patch = p[2];
	info->revchar = p[3];
	info->release = peek_be32(&p[4]);
	info->build = peek_be32(&p[8]);

	return GGEP_OK;
}

/**
 * From a sequence of IP:port addresses, fill a set of GGEP extensions:
 *
 *		N and N_TLS
 *
 * for the given network type.
 *
 * @param gs		the GGEP stream to which extensions are written
 * @param hseq		sequence of IP:port (IPv4 and IPv6 mixed)
 * @param net		network address type
 * @param name		name of GGEP extension for addresses
 * @param name_tls	name of GGEP extension for TLS
 * @param evec		optional exclusion address vector
 * @param ecnt		length of evec[]
 * @param count		input: max amount of items to generate, output: items sent
 * @param cobs		whether COBS encoding is required
 *
 * @return TRUE on success, FALSE on write errors.
 */
static gboolean
ggept_ip_seq_append_net(ggep_stream_t *gs,
	const sequence_t *hseq, enum net_type net,
	const char *name, const char *name_tls,
	const gnet_host_t *evec, size_t ecnt, size_t *count, gboolean cobs)
{
	guchar *tls_bytes = NULL;
	unsigned tls_length;
	size_t tls_size = 0, tls_index = 0;
	gboolean status = FALSE;
	unsigned flags = 0;
	size_t hcnt;
	const char *current_extension;
	sequence_iter_t *iter = NULL;
	size_t max_items = *count;

	g_assert(gs != NULL);
	g_assert(name != NULL);
	g_assert(hseq != NULL);

	hcnt = sequence_count(hseq);

	if (0 == hcnt) {
		status = TRUE;
		goto done;
	}

	tls_size = (hcnt + 7) / 8;
	tls_bytes = name_tls != NULL ? walloc0(tls_size) : NULL;
	tls_index = tls_length = 0;

	/*
	 * We only attempt to deflate IPv6 vectors, since IPv4 does not bring
	 * enough redundancy to be worth it: 180 bytes of data for 30 IPv4
	 * addresses typically compress to 175 bytes.  Hardly interesting.
	 */

	flags |= (NET_TYPE_IPV6 == net) ? GGEP_W_DEFLATE : 0;
	flags |= cobs ? GGEP_W_COBS : 0;

	/*
	 * We use GGEP_W_STRIP to make sure the extension is removed if empty.
	 */

	current_extension = name;

	if (!ggep_stream_begin(gs, name, GGEP_W_STRIP | flags))
		goto done;

	iter = sequence_forward_iterator(hseq);

	while (sequence_iter_has_next(iter) && tls_index < max_items) {
		host_addr_t addr;
		guint16 port;
		char buf[18];
		size_t len;
		const gnet_host_t *h = sequence_iter_next(iter);

		if (net != gnet_host_get_net(h))
			continue;

		/*
		 * See whether we need to skip that host.
		 */

		if (evec != NULL) {
			size_t i;

			for (i = 0; i < ecnt; i++) {
				if (gnet_host_eq(h, &evec[i]))
					goto next;
			}
		}

		addr = gnet_host_get_addr(h);
		port = gnet_host_get_port(h);

		host_ip_port_poke(buf, addr, port, &len);
		if (!ggep_stream_write(gs, buf, len))
			goto done;

		/*
		 * Record in bitmask whether host is known to support TLS.
		 */

		if (name_tls != NULL && tls_cache_lookup(addr, port)) {
			tls_bytes[tls_index >> 3] |= 0x80U >> (tls_index & 7);
			tls_length = (tls_index >> 3) + 1;
		}
		tls_index++;
	next:
		continue;
	}

	if (!ggep_stream_end(gs))
		goto done;

	if (tls_length > 0) {
		unsigned gflags = cobs ? GGEP_W_COBS : 0;
		g_assert(name_tls != NULL);
		current_extension = name_tls;
		if (!ggep_stream_pack(gs, name_tls, tls_bytes, tls_length, gflags))
			goto done;
	}

	status = TRUE;

done:
	if (!status) {
		g_carp("unable to add GGEP \"%s\"", current_extension);
	}

	*count = tls_index;

	sequence_iterator_release(&iter);
	WFREE_NULL(tls_bytes, tls_size);
	return status;
}

/**
 * From a sequence of IP:port addresses fill two sets of GGEP extensions:
 *
 *    NAME and NAME_TLS for IPv4 addresses
 *    NAME6 and NAME6_TLS for IPv6 addresses
 *
 * @param gs		the GGEP stream to which extensions are written
 * @param hseq		sequence of IP:port (IPv4 and IPv6 mixed)
 * @param name		name of GGEP extension for IPv4 addresses
 * @param name_tls	name of GGEP extension for TLS vector of IPv4 addresses
 * @param name6		name of GGEP extension for IPv6 addresses
 * @param name6_tls	name of GGEP extension for TLS vector of IPv6 addresses
 * @param evec		optional exclusion address vector
 * @param ecnt		length of evec[]
 * @param max_items	maximum amount of items to include, (size_t) -1 means all
 * @param cobs		whether COBS encoding is required
 *
 * @return GGEP_OK on success, GGEP_BAD_SIZE on write errors.
 */
static ggept_status_t
ggept_ip_seq_append(ggep_stream_t *gs,
	const sequence_t *hseq,
	const char *name, const char *name_tls,
	const char *name6, const char *name6_tls,
	const gnet_host_t *evec, size_t ecnt, size_t max_items, gboolean cobs)
{
	size_t count = max_items;

	if (name != NULL && count != 0) {
		if (
			!ggept_ip_seq_append_net(gs, hseq, NET_TYPE_IPV4,
				name, name_tls, evec, ecnt, &count, cobs)
		) {
			return GGEP_BAD_SIZE;
		}
	}

	g_assert(count <= max_items);
	count = max_items - count;

	if (name6 != NULL && count != 0) {
		if (
			!ggept_ip_seq_append_net(gs, hseq, NET_TYPE_IPV6,
				name6, name6_tls, evec, ecnt, &count, cobs)
		) {
			return GGEP_BAD_SIZE;
		}
	}
	return GGEP_OK;
}

/**
 * Emit vector of IP:port addresses for "IPP".
 *
 * @param gs		the GGEP stream to which extensions are written
 * @param hvec		vector of IP:port (IPv4 and IPv6 mixed)
 * @param hcnt		length of hvec[]
 * @param evec		exclusion address vector
 * @param ecnt		length of evec[]
 * @param add_ipv6	whether IPv6 addresses are requested
 * @param no_ipv4	whether IPv4 addresses should be excluded
 */
ggept_status_t
ggept_ipp_pack(ggep_stream_t *gs, const gnet_host_t *hvec, size_t hcnt,
	const gnet_host_t *evec, size_t ecnt,
	gboolean add_ipv6, gboolean no_ipv4)
{
	vector_t v = vector_create(deconstify_gpointer(hvec), sizeof *hvec, hcnt);
	sequence_t hseq;

	sequence_fill_from_vector(&hseq, &v);

	return ggept_ip_seq_append(gs, &hseq,
		no_ipv4 ? NULL : GGEP_NAME(IPP), GGEP_NAME(IPP_TLS),
		add_ipv6 ? GGEP_NAME(IPP6) : NULL, GGEP_NAME(IPP6_TLS),
		evec, ecnt, (size_t) -1, FALSE);
}

/**
 * Emit vector of IP:port addresses for "DHTIPP".
 *
 * @param gs		the GGEP stream to which extensions are written
 * @param hvec		vector of IP:port (IPv4 and IPv6 mixed)
 * @param hcnt		length of hvec[]
 * @param add_ipv6	whether IPv6 addresses are requested
 * @param no_ipv4	whether IPv4 addresses should be excluded
 */
ggept_status_t
ggept_dhtipp_pack(ggep_stream_t *gs, const gnet_host_t *hvec, size_t hcnt,
	gboolean add_ipv6, gboolean no_ipv4)
{
	vector_t v = vector_create(deconstify_gpointer(hvec), sizeof *hvec, hcnt);
	sequence_t hseq;

	sequence_fill_from_vector(&hseq, &v);

	return ggept_ip_seq_append(gs, &hseq,
		no_ipv4 ? NULL : GGEP_NAME(IPP), NULL,
		add_ipv6 ? GGEP_NAME(IPP6) : NULL, NULL,
		NULL, 0, (size_t) -1, FALSE);
}

/**
 * Emit sequence of IP:port addresses for "PUSH".
 *
 * @param gs		the GGEP stream to which extensions are written
 * @param hseq		sequence of IP:port (IPv4 and IPv6 mixed)
 * @param max		maximum amount of entries to add
 * @param flags		a combination of QHIT_F_* flags
 */
ggept_status_t
ggept_push_pack(ggep_stream_t *gs, const sequence_t *hseq, size_t max,
	unsigned flags)
{
	return ggept_ip_seq_append(gs, hseq,
		(flags & QHIT_F_IPV6_ONLY) ? NULL : GGEP_NAME(PUSH),
		GGEP_NAME(PUSH_TLS),
		(flags & QHIT_F_IPV6) ? GGEP_NAME(PUSH6) : NULL, GGEP_NAME(PUSH6_TLS),
		NULL, 0, max, FALSE);
}

/**
 * Emit sequence of IP:port addresses for "A" in HEAD pongs.
 *
 * @param gs		the GGEP stream to which extensions are written
 * @param hvec		vector of IP:port (IPv4 and IPv6 mixed)
 * @param hcnt		length of hvec[]
 */
ggept_status_t
ggept_a_pack(ggep_stream_t *gs, const gnet_host_t *hvec, size_t hcnt)
{
	vector_t v = vector_create(deconstify_gpointer(hvec), sizeof *hvec, hcnt);
	sequence_t hseq;

	sequence_fill_from_vector(&hseq, &v);

	return ggept_ip_seq_append(gs, &hseq,
		GGEP_NAME(A), GGEP_NAME(T),
		GGEP_NAME(A6), GGEP_NAME(T6),
		NULL, 0, (size_t) -1, FALSE);
}

/**
 * Emit sequence of IP:port addresses for "ALT" in query hits.
 *
 * @param gs		the GGEP stream to which extensions are written
 * @param hvec		vector of IP:port (IPv4 and IPv6 mixed)
 * @param hcnt		length of hvec[]
 * @param flags		a combination of QHIT_F_* flags
 */
ggept_status_t
ggept_alt_pack(ggep_stream_t *gs, const gnet_host_t *hvec, size_t hcnt,
	unsigned flags)
{
	vector_t v = vector_create(deconstify_gpointer(hvec), sizeof *hvec, hcnt);
	sequence_t hseq;

	sequence_fill_from_vector(&hseq, &v);

	/* This needs COBS encoding */

	return ggept_ip_seq_append(gs, &hseq,
		(flags & QHIT_F_IPV6_ONLY) ? NULL : GGEP_NAME(ALT), GGEP_NAME(ALT_TLS),
		(flags & QHIT_F_IPV6) ? GGEP_NAME(ALT6) : NULL, GGEP_NAME(ALT6_TLS),
		NULL, 0, (size_t) -1, TRUE);
}

static ggept_status_t
ggept_ip_vec_extract(const extvec_t *exv,
	gnet_host_vec_t **hvec, enum net_type net)
{
	int len;
	int ilen;

	g_assert(exv);
	g_assert(hvec);
	g_assert(EXT_GGEP == exv->ext_type);
	g_assert(NET_TYPE_IPV4 == net || NET_TYPE_IPV6 == net);

	len = ext_paylen(exv);
	ilen = NET_TYPE_IPV4 == net ? 6 : 18;	/* IP + port */

	if (len <= 0)
		return GGEP_INVALID;

	if (len % ilen != 0)
		return GGEP_INVALID;

	if (hvec) {
		gnet_host_vec_t *vec = *hvec;
		const char *p;
		guint n, i;

		vec = NULL == vec ? gnet_host_vec_alloc() : vec;
		n = len / ilen;
		n = MIN(n, 255);	/* n_ipv4 and n_ipv6 are guint8 */

		g_assert(n > 0);

		if (NET_TYPE_IPV4 == net) {
			if (vec->n_ipv4 != 0)
				return GGEP_DUPLICATE;
			vec->n_ipv4 = n;
			vec->hvec_v4 = walloc(n * sizeof vec->hvec_v4[0]);
		} else {
			if (vec->n_ipv6 != 0)
				return GGEP_DUPLICATE;
			vec->n_ipv6 = n;
			vec->hvec_v6 = walloc(n * sizeof vec->hvec_v6[0]);
		}

		p = ext_payload(exv);
		for (i = 0; i < n; i++) {
			/* IPv4 address (BE) or IPv6 address (BE) + Port (LE) */
			if (NET_TYPE_IPV4 == net) {
				memcpy(&vec->hvec_v4[i].data, p, 6);
				p += 6;
			} else {
				memcpy(&vec->hvec_v6[i].data, p, 18);
				p += 18;
			}
		}
		*hvec = vec;
	}

	return GGEP_OK;
}

/**
 * Extract vector of IP:port alternate locations.
 *
 * The `hvec' pointer is filled with a dynamically allocated vector.
 * Unless GGEP_OK is returned, no memory allocation takes place.
 *
 * If *hvec is not NULL, it is filled with new hosts provided that there were
 * no hosts for that kind yet within the vector.
 *
 * @param exv	the extension vector
 * @param hvec	pointer is filled with a dynamically allocated vector.
 * @param net	type of network addresses expected in the extension
 *
 * @return GGEP_OK on success
 */
ggept_status_t
ggept_alt_extract(const extvec_t *exv,
	gnet_host_vec_t **hvec, enum net_type net) 
{
	g_assert(exv->ext_type == EXT_GGEP);
	g_assert(exv->ext_token == EXT_T_GGEP_ALT ||
		exv->ext_token == EXT_T_GGEP_ALT6);

	return ggept_ip_vec_extract(exv, hvec, net);
}

/**
 * Extract vector of IP:port push proxy locations.
 *
 * The `hvec' pointer is filled with a dynamically allocated vector.
 * Unless GGEP_OK is returned, no memory allocation takes place.
 *
 * If *hvec is not NULL, it is filled with new hosts provided that there were
 * no hosts for that kind yet within the vector.
 *
 * @param exv	the extension vector
 * @param hvec	pointer is filled with a dynamically allocated vector.
 * @param net	type of network addresses expected in the extension
 *
 * @return GGEP_OK on success
 */
ggept_status_t
ggept_push_extract(const extvec_t *exv,
	gnet_host_vec_t **hvec, enum net_type net)
{
	g_assert(exv->ext_type == EXT_GGEP);
	g_assert(exv->ext_token == EXT_T_GGEP_PUSH);

	return ggept_ip_vec_extract(exv, hvec, net);
}

/**
 * Extract an UTF-8 encoded string into the supplied buffer.
 *
 * @returns extraction status: only when GGEP_OK is returned will we have
 * extracted something in the supplied buffer.
 */
ggept_status_t
ggept_utf8_string_extract(const extvec_t *exv, char *buf, size_t len)
{
	int tlen;

	g_assert(size_is_non_negative(len));
	g_assert(exv->ext_type == EXT_GGEP);

	/*
	 * The payload should not contain a NUL.
	 * We only copy up to the first NUL.
	 * The empty string is accepted.
	 */

	tlen = ext_paylen(exv);
	if (tlen < 0 || UNSIGNED(tlen) >= len)
		return GGEP_INVALID;

	clamp_strncpy(buf, len, ext_payload(exv), tlen);

	if (!utf8_is_valid_string(buf))
		return GGEP_INVALID;

	return GGEP_OK;
}

/**
 * Extract hostname of the "HNAME" extension into the supplied buffer.
 *
 * @returns extraction status: only when GGEP_OK is returned will we have
 * extracted something in the supplied buffer.
 */
ggept_status_t
ggept_hname_extract(const extvec_t *exv, char *buf, int len)
{
	g_assert(len >= 0);
	g_assert(exv->ext_type == EXT_GGEP);
	g_assert(exv->ext_token == EXT_T_GGEP_HNAME);

	if (GGEP_OK != ggept_utf8_string_extract(exv, buf, len))
		return GGEP_INVALID;

	/*
	 * Make sure the full string qualifies as hostname and is not an
	 * IP address.
	 */
	{
		const char *endptr;
		host_addr_t addr;

		if (
			!string_to_host_or_addr(buf, &endptr, &addr) ||
			'\0' != *endptr ||
			is_host_addr(addr)
		) {
			return GGEP_INVALID;
		}
	}

	return GGEP_OK;
}

/**
 * Encodes a variable-length integer. This encoding is equivalent to
 * little-endian encoding whereas trailing zeros are discarded.
 *
 * @param v		The value to encode.
 * @param data  Must point to a sufficiently large buffer. At maximum
 *				8 bytes are required.
 *
 * @return the length in bytes of the encoded variable-length integer.
 */
static inline int
ggep_vlint_encode(guint64 v, char *data)
{
	char *p;

	for (p = data; v != 0; v >>= 8)	{
		*p++ = v & 0xff;
	}

	return p - data;
}

/**
 * Decodes a variable-length integer. This encoding is equivalent to
 * little-endian encoding whereas trailing zeros are discarded.
 *
 * @param data The payload to decode.
 * @param len The length of data in bytes.
 *
 * @return The decoded value.
 */
static inline guint64
ggep_vlint_decode(const char *data, size_t len)
{
	guint64 v;
	guint i;

	v = 0;
	if (len <= 8) {
		for (i = 0; i < len; i++) {
			v |= (((guint64) data[i]) & 0xff) << (i * 8);
		}
	}
	return v;
}

/**
 * Extract filesize length into `filesize' from extension encoded in variable-
 * length little endian with leading zeroes stripped.
 *
 * This is the format used by the payload of GGEP "LF" for instance.
 */
ggept_status_t
ggept_filesize_extract(const extvec_t *exv, guint64 *filesize)
{
	guint64 fs;
	size_t len;

	g_assert(exv->ext_type == EXT_GGEP);

	len = ext_paylen(exv);
	if (len < 1 || len > 8) {
		return GGEP_INVALID;
	}
	fs = ggep_vlint_decode(ext_payload(exv), len);
	if (0 == fs) {
		return GGEP_INVALID;
	}
	if (filesize) {
		*filesize = fs;
	}
	return GGEP_OK;
}

/**
 * Extract IPv6 address into `addr' from GGEP "GTKG.IPV6" or "6" extensions.
 * When "addr" is NULL, simply validates the payload length.
 */
ggept_status_t
ggept_gtkg_ipv6_extract(const extvec_t *exv, host_addr_t *addr)
{
	size_t len;

	g_assert(exv->ext_type == EXT_GGEP);
	g_assert(exv->ext_token == EXT_T_GGEP_GTKG_IPV6 ||
		exv->ext_token == EXT_T_GGEP_6);

	len = ext_paylen(exv);
	if (0 != len && len < 16)
		return GGEP_INVALID;

	if (addr != NULL) {
		if (0 == len) {
			*addr = zero_host_addr;
		} else {
			g_assert(len >= 16);
			*addr = host_addr_peek_ipv6(ext_payload(exv));
		}
	}

	return GGEP_OK;
}


/**
 * Encode `filesize' in variable-length little endian, with leading zeroes
 * stripped, into `data'.
 *
 * This is used in extensions such as GGEP "LF" which carry the file length.
 *
 * @param filesize	The filesize to encode.
 * @param data		A buffer of at least 8 bytes.
 *
 * @return the amount of bytes written.
 */
guint
ggept_filesize_encode(guint64 filesize, char *data)
{
	return ggep_vlint_encode(filesize, data);
}

/**
 * Extract unsigned (32-bit) quantity encoded as variable-length little-endian.
 */
ggept_status_t
ggept_uint32_extract(const extvec_t *exv, guint32 *val)
{
	guint32 v;
	size_t len;

	g_assert(exv->ext_type == EXT_GGEP);

	len = ext_paylen(exv);
	if (len > 4) {
		return GGEP_INVALID;
	}
	v = ggep_vlint_decode(ext_payload(exv), len);
	if (val != NULL) {
		*val = v;
	}
	return GGEP_OK;
}

/**
 * Extract daily uptime into `uptime', from the GGEP "DU" extensions.
 */
ggept_status_t
ggept_du_extract(const extvec_t *exv, guint32 *uptime)
{
	g_assert(exv->ext_type == EXT_GGEP);
	g_assert(exv->ext_token == EXT_T_GGEP_DU);

	return ggept_uint32_extract(exv, uptime);
}

/**
 * Encode `uptime' for the GGEP "DU" extension into `data'.
 *
 * @param uptime The uptime (in seconds) to encode.
 * @param data A buffer of at least 4 bytes.
 * @return the amount of chars written.
 */
guint
ggept_du_encode(guint32 uptime, char *data)
{
	return ggep_vlint_encode(uptime, data);
}

/**
 * Encode `media_type' for the GGEP "M" extension into `data'.
 *
 * @param mtype	The media type mask
 * @param data	A buffer of at least 4 bytes.
 *
 * @return the amount of chars written.
 */
guint
ggept_m_encode(guint32 mtype, char *data)
{
	return ggep_vlint_encode(mtype, data);
}


ggept_status_t
ggept_ct_extract(const extvec_t *exv, time_t *stamp_ptr)
{
	guint64 v;
	size_t len;

	g_assert(exv->ext_type == EXT_GGEP);
	g_assert(exv->ext_token == EXT_T_GGEP_CT);

	len = ext_paylen(exv);
	if (len > 8) {
		return GGEP_INVALID;
	}
	v = ggep_vlint_decode(ext_payload(exv), len);
	if (stamp_ptr) {
		*stamp_ptr = MIN(v, TIME_T_MAX);
	}
	return GGEP_OK;
}

/**
 * Encode `timestamp' for the GGEP "CT" extension into `data'.
 *
 * @param timestamp The timestamp (seconds since epoch) to encode.
 * @param data A buffer of at least 8 bytes.
 * @return the amount of chars written.
 */
guint
ggept_ct_encode(time_t timestamp, char *data)
{
	return ggep_vlint_encode(timestamp, data);
}

/* vi: set ts=4 sw=4 cindent: */
