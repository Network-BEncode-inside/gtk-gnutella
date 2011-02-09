/*
 * $Id$
 *
 * Copyright (c) 2008-2009, Raphael Manfredi
 * Copyright (c) 2003-2008, Christian Biere
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
 * @ingroup lib
 * @file
 *
 * Stringification routines.
 *
 * @author Raphael Manfredi
 * @date 2008-2009
 * @author Christian Biere
 * @date 2003-2008
 */

#ifndef _stringify_h_
#define _stringify_h_

#include "tm.h"

/*
 * Macros to determine the maximum buffer size required to hold a
 * NUL-terminated string.
 */
#define IPV4_ADDR_BUFLEN	(sizeof "255.255.255.255")
#define IPV6_ADDR_BUFLEN \
	  (sizeof "0001:0203:0405:0607:0809:1011:255.255.255.255")
#define TIMESTAMP_BUF_LEN	(sizeof "9999-12-31 23:59:61")

/*
 * How many bytes do we need to stringify an unsigned quantity in decimal
 * form, including the trailing NUL?
 *
 * To represent a decimal number x, one needs 1 + E(log(x)) digits, E(x)
 * being the integer part of x, and log(x) = ln(x) / ln(10)
 *
 * For a power of 2, this becomes:
 *
 * log(2^n) = log(2) * n.
 * log(2) = 0.301029995, which can be approximated by 146/485 (larger value).
 */
#define BIT_DEC_BUFLEN(n)	(2 + ((n) * 146) / 485)		/* 2 = 1 + NUL */
#define TYPE_DEC_BUFLEN(t)	BIT_DEC_BUFLEN(sizeof(t) * CHAR_BIT)
#define TYPE_HEX_BUFLEN(t)	(1 + sizeof(t) * (CHAR_BIT / 4))

/*
 * The following include space for NUL, too.
 */
#define UINT8_DEC_BUFLEN	TYPE_DEC_BUFLEN(guint8)
#define UINT16_DEC_BUFLEN	TYPE_DEC_BUFLEN(guint16)
#define UINT32_DEC_BUFLEN	TYPE_DEC_BUFLEN(guint32)
#define UINT64_DEC_BUFLEN	TYPE_DEC_BUFLEN(guint64)
#define OFF_T_DEC_BUFLEN	TYPE_DEC_BUFLEN(fileoffset_t)
#define TIME_T_DEC_BUFLEN	TYPE_DEC_BUFLEN(time_t)
#define SIZE_T_DEC_BUFLEN	TYPE_DEC_BUFLEN(size_t)
#define USHRT_DEC_BUFLEN	TYPE_DEC_BUFLEN(unsigned short)
#define UINT_DEC_BUFLEN		TYPE_DEC_BUFLEN(unsigned int)
#define ULONG_DEC_BUFLEN	TYPE_DEC_BUFLEN(unsigned long)

#define UINT8_HEX_BUFLEN	TYPE_HEX_BUFLEN(guint8)
#define UINT16_HEX_BUFLEN	TYPE_HEX_BUFLEN(guint16)
#define UINT32_HEX_BUFLEN	TYPE_HEX_BUFLEN(guint32)
#define UINT64_HEX_BUFLEN	TYPE_HEX_BUFLEN(guint64)
#define POINTER_BUFLEN		TYPE_HEX_BUFLEN(unsigned long)

#define HOST_ADDR_BUFLEN		(MAX(IPV4_ADDR_BUFLEN, IPV6_ADDR_BUFLEN))
#define HOST_ADDR_PORT_BUFLEN	(HOST_ADDR_BUFLEN + sizeof ":[65535]")

size_t int32_to_string_buf(gint32 v, char *dst, size_t size);
size_t uint32_to_string_buf(guint32 v, char *dst, size_t size);
size_t uint64_to_string_buf(guint64 v, char *dst, size_t size);
size_t fileoffset_t_to_string_buf(fileoffset_t v, char *dst, size_t size);
size_t size_t_to_string_buf(size_t v, char *dst, size_t size);
size_t pointer_to_string_buf(const void *ptr, char *dst, size_t size);
const char *uint32_to_string(guint32);
const char *uint64_to_string(guint64);
const char *uint64_to_string2(guint64);
const char *fileoffset_t_to_string(fileoffset_t);
const char *size_t_to_string(size_t);
const char *pointer_to_string(const void *);
const char *filesize_to_string(filesize_t);
const char *filesize_to_string2(filesize_t);
const char *ipv6_to_string(const guint8 *ipv6);
size_t ipv6_to_string_buf(const guint8 *ipv6, char *dst, size_t size);

char *hex_escape(const char *name, gboolean strict);
char *control_escape(const char *s);
const char *lazy_string_to_printf_escape(const char *src);

/*
 * Time string conversions
 */
const char *short_time(time_delta_t s);
const char *short_time_ascii(time_delta_t t);
size_t compact_time_to_buf(time_delta_t t, char *dst, size_t size);
const char *compact_time(time_delta_t t);
const char *compact_time2(time_delta_t t);
const char *short_uptime(time_delta_t s);
size_t time_locale_to_string_buf(time_t date, char *dst, size_t size);
size_t time_t_to_string_buf(time_t v, char *dst, size_t size);
const char *time_t_to_string(time_t);

#endif /* _stringify_h_ */

/* vi: set ts=4 sw=4 cindent: */
