/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
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
 * Misc functions.
 *
 * This misc.[ch] provides several miscellaneous small routines & macros for:
 *
 * - Array size determination
 * - Flag handling
 * - Sorting constants
 * - Network related string routines
 * - Date string conversions
 * - Time string conversions
 * - Size string conversions
 * - SHA1<->base32 string conversion
 * - Tests
 * - Stuff...
 *
 * @author Raphael Manfredi
 * @date 2001-2003
 */

#ifndef _misc_h_
#define _misc_h_

#include "common.h"

#include "fs_free_space.h"
#include "tm.h"
#include "vmm.h"

#define SIZE_FIELD_MAX		64	/**< Max size of sprintf-ed size quantity */
#define GUID_RAW_SIZE		16	/**< Binary representation of 128 bits */
#define GUID_HEX_SIZE		32	/**< Hexadecimal GUID representation */
#define GUID_BASE32_SIZE	26	/**< base32 GUID representation */

typedef struct short_string {
	char str[SIZE_FIELD_MAX];
} short_string_t;

static inline int
is_dir_separator(int c)
{
	return '/' == c || G_DIR_SEPARATOR == c;
}

/**
 * Converts an integer to a single hexadecimal ASCII digit. The are no checks,
 * this is just a convenience function.
 *
 * @param x An integer between 0 and 15.
 * @return The ASCII character corresponding to the hex digit [0-9a-f].
 */
static inline guchar
hex_digit(guchar x)
{
	extern const char hex_alphabet_lower[];
	return hex_alphabet_lower[x & 0xf]; 
}

#if !GLIB_CHECK_VERSION(2,4,0)
static inline WARN_UNUSED_RESULT const char *
g_strip_context(const char *id, const char *val)
{
	const char *s;

	s = id != val ? NULL : strchr(id, '|');
	return s ? ++s : val;
}
#endif /* GLib < 2.4.0 */

static inline WARN_UNUSED_RESULT char *
skip_dir_separators(const char *s)
{
	while (is_dir_separator(s[0]))
		s++;

	return deconstify_gchar(s);
}

/*
 * Determine the length of string literals
 */
#define CONST_STRLEN(x) (sizeof(x) - 1)

/*
 * Network related string routines
 */
const char *local_hostname(void);
#define port_is_valid(port) (port != 0)

/*
 * Size string conversions
 */
const char *short_frequency(guint64 freq);
const char *short_size(guint64 size, gboolean metric);
const char *short_html_size(guint64 size, gboolean metric);
const char *short_kb_size(guint64 size, gboolean metric);
const char *short_kb_size2(guint64 size, gboolean metric);
const char *short_rate(guint64 rate, gboolean metric);
const char *short_byte_size(guint64 size, gboolean metric);
const char *short_byte_size2(guint64 size, gboolean metric);
const char *compact_size(guint64 size, gboolean metric);
const char *compact_size2(guint64 size, gboolean metric);
const char *compact_rate(guint64 rate, gboolean metric);
const char *compact_kb_size(guint32 size, gboolean metric);
const char *nice_size(guint64 size, gboolean metric);
char *short_value(char *buf, size_t size, guint64 v, gboolean metric);
char *compact_value(char *buf, size_t size, guint64 v, gboolean metric);

size_t short_byte_size_to_buf(guint64 size, gboolean metric, char *, size_t);
size_t short_kb_size_to_buf(guint64 size, gboolean metric, char *, size_t);
size_t short_size_to_string_buf(guint64 size, gboolean metric, char *, size_t);

short_string_t short_rate_get_string(guint64 rate, gboolean metric);

/*
 * SHA1<->base32 string conversion
 */
typedef struct sha1 {
	char data[SHA1_RAW_SIZE];
} sha1_t;

#define SHA1_URN_LENGTH	(CONST_STRLEN("urn:sha1:") + SHA1_BASE32_SIZE)

const char *sha1_to_string(const struct sha1 *sha1);
const char *sha1_to_urn_string(const struct sha1 *);
size_t sha1_to_urn_string_buf(const struct sha1 *, char *dst, size_t size);
char *sha1_to_base32_buf(const struct sha1 *, char *dst, size_t size);
const char *sha1_base32(const struct sha1 *);
const struct sha1 *base32_sha1(const char *base32);

static inline int
sha1_cmp(const struct sha1 *a, const struct sha1 *b)
{
	return memcmp(a, b, SHA1_RAW_SIZE);
}

/*
 * TTH <-> base32 string conversion
 */
typedef struct tth {
	char data[TTH_RAW_SIZE];
} tth_t;

#define TTH_URN_LENGTH	(CONST_STRLEN("urn:ttroot:") + TTH_BASE32_SIZE)

const char *tth_base32(const struct tth *);
const struct tth *base32_tth(const char *base32);
const char *tth_to_urn_string(const struct tth *);
size_t tth_to_urn_string_buf(const struct tth *, char *dst, size_t size);
char *tth_to_base32_buf(const struct tth *, char *dst, size_t size);


const char *bitprint_to_urn_string(const struct sha1 *, const struct tth *);

/*
 * GUID<->hex string conversion
 */
struct guid;

const char *guid_hex_str(const struct guid *);
gboolean hex_to_guid(const char *, struct guid *);
size_t guid_to_string_buf(const struct guid *, char *, size_t);
const char *guid_to_string(const struct guid *);

/*
 * GUID<->base32 string conversion
 */
const char *guid_base32_str(const struct guid *);
const struct guid *base32_to_guid(const char *);

/*
 * Generic binary to hexadecimal conversion.
 */
size_t bin_to_hex_buf(const void *data, size_t len, char *dst, size_t size);

/*
 * Tests
 */
gboolean is_directory(const char *pathname);
gboolean is_regular(const char *pathname);
gboolean is_symlink(const char *pathname);
int is_same_file(const char *, const char *);

/**
 * Tries to extrace the file mode from a struct dirent. Not all systems
 * support this, in which case zero is returned. Types other than regular
 * files, directories and symlinks are ignored and gain a value of zero
 * as well.
 */
static inline mode_t
dir_entry_mode(const struct dirent *dir_entry)
{
	g_assert(dir_entry);
#ifdef HAS_DIRENT_D_TYPE
	switch (dir_entry->d_type) {
	case DT_DIR:	return S_IFDIR;
	case DT_LNK:	return S_IFLNK;
	case DT_REG:	return S_IFREG;
	case DT_CHR:	return S_IFCHR;
	case DT_BLK:	return S_IFBLK;
	case DT_FIFO:	return S_IFIFO;
#if defined(DT_WHT) && defined(S_IFWHT)
	case DT_WHT:	return S_IFWHT;
#endif	/* DT_WHT */
#if defined(DT_SOCK) && defined(S_IFSOCK)
	case DT_SOCK:	return S_IFSOCK;
#endif	/* DT_SOCK */
	}
#endif	/* HAS_DIRENT_WITH_D_TYPE */
	return 0;
}

/*
 * Stuff
 */

void misc_init(void);
void misc_close(void);

size_t strchomp(char *str, size_t len);
int hex2int(guchar c);
gboolean is_printable(const char *buf, int len);
void dump_hex(FILE *, const char *, gconstpointer, int);
void dump_string(FILE *out, const char *str, size_t len, const char *trailer);
gboolean is_printable_iso8859_string(const char *s);
void locale_strlower(char *, const char *);
size_t common_leading_bits(
	gconstpointer k1, size_t k1bits, gconstpointer k2, size_t k2bits);
float force_range(float value, float min, float max);
const char *short_filename(const char *fullname);
char *data_hex_str(const char *data, size_t len);

#if defined(S_IROTH) && defined(S_IXOTH)
/* 0755 */
#define DEFAULT_DIRECTORY_MODE (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)
#else
/* 0750 */
#define DEFAULT_DIRECTORY_MODE (S_IRWXU | S_IRGRP | S_IXGRP)
#endif /* S_IROTH && S_IXOTH */

int create_directory(const char *dir, mode_t mode);

char *is_strprefix(const char *s, const char *prefix) WARN_UNUSED_RESULT;
char *is_strcaseprefix(const char *s, const char *prefix) WARN_UNUSED_RESULT;
gboolean is_strsuffix(const char *str, size_t len, const char *suffix);
size_t html_escape(const char *src, char *dst, size_t dst_size);
guint32 html_decode_entity(const char *src, const char **endptr);

void normalize_dir_separators(char *);
size_t memcmp_diff(const void *a, const void *b, size_t n);

unsigned pointer_hash_func(const void *p);

/**
 * An strcpy() that returns the length of the copied string.
 */
static inline size_t
strcpy_len(char *dest, const char *src)
{
	const char *p = src;
	char *q = dest;
	int c;

	g_assert(dest != NULL);

	if (NULL == src)
		return 0;
	
	while ((c = *p++))
		*q++ = c;

	*q = '\0';

	return q - dest;
}

/**
 * Determines the length of a NUL-terminated string looking only at the first
 * "src_size" bytes. If src[0..size] contains no NUL byte, "src_size" is
 * returned. Otherwise, the returned value is identical to strlen(str). Thus,
 * it is safe to pass a possibly non-terminated buffer.
 *
 * @param src An initialized buffer.
 * @param src_size The size of src in number of bytes. IF AND ONLY IF,
 *        src is NUL-terminated, src_size may exceed the actual buffer length.
 * @return The number of bytes in "src" before the first found NUL or src_size
 *		   if there is no NUL.
 */
static inline size_t
clamp_strlen(const char *src, size_t src_size)
{
	const char *p;

	/* @NOTE: memchr() is intentionally NOT used because 'src_size' is allowed
	 *        to exceed the size of the memory object 'src'.
	 */
	for (p = src; src_size-- > 0 && '\0' != *p; p++)
		continue;

	return p - src;
}

/**
 * Copies at most MIN(dst_size, src_len) bytes from "src" to "dst".
 *
 * @param dst the destination buffer.
 * @param dst_size the size of dst in number of bytes. 
 * @param src the source buffer. 
 * @param src_len the length of src in number of bytes.
 *
 * @return The number of copied bytes.
 */
static inline size_t
clamp_memcpy(char *dst, size_t dst_size, const char *src, size_t src_len)
{
	size_t n;

	n = MIN(dst_size, src_len);
	memcpy(dst, src, n);
	return n;
}

/**
 * Sets MIN(dst_size, src_len) bytes starting at dst to 'c'.
 *
 * @param dst the destination buffer.
 * @param dst_size the size of dst in number of bytes.
 * @param c the value to set each byte to. 
 * @param n the number of bytes to set.
 *
 * @return The number of set bytes.
 */
static inline size_t
clamp_memset(char *dst, size_t dst_size, char c, size_t n)
{
	n = MIN(dst_size, n);
	memset(dst, c, n);
	return n;
}

/**
 * Copies at most MIN(dst_size - 1, src_len) characters from the buffer "src"
 * to the buffer "dst", ensuring the resulting string in "dst" is
 * NUL-terminated and truncating it if necessary. If "src_len" is (size_t)-1,
 * "src" must be NUL-terminated, otherwise the first "src_len" bytes of "src"
 * must be initialized but a terminating NUL is not necessary.
 *
 * @NOTE: The 'dst' buffer is NOT padded with NUL-bytes.
 *
 * @param dst the destination buffer.
 * @param dst_size the size of dst in number of bytes. 
 * @param src a NUL-terminated string or at an initialized buffer of least
 *        "src_len" bytes.
 * @param src_len the length of src in number of bytes to copy at maximum. May
 *        be (size_t)-1 if "src" is NUL-terminated.
 *
 * @return The length of the resulting string in number of bytes.
 */
static inline size_t
clamp_strncpy(char *dst, size_t dst_size, const char *src, size_t src_len)
{
	if (dst_size-- > 0) {
		size_t n;

		if ((size_t) -1 == src_len) {
			src_len = clamp_strlen(src, dst_size);
		}
		n = clamp_memcpy(dst, dst_size, src, src_len);
		dst[n] = '\0';
		return n;
	} else {
		return 0;
	}
}

/**
 * Copies at most "dst_size - 1" characters from the NUL-terminated string
 * "src" to the buffer "dst", ensuring the resulting string in "dst" is
 * NUL-terminated and truncating it if necessary.
 *
 * @NOTE: The 'dst' buffer is NOT padded with NUL-bytes.
 *
 * @param dst the destination buffer.
 * @param dst_size the size of dst in number of bytes. 
 * @param src a NUL-terminated string.
 *
 * @return The length of the resulting string in number of bytes.
 */
static inline size_t
clamp_strcpy(char *dst, size_t dst_size, const char *src)
{
	return clamp_strncpy(dst, dst_size, src, (size_t) -1);
}


static inline const char *
NULL_STRING(const char *s)
{
	return NULL != s ? s : "(null)";
}

static inline const char *
EMPTY_STRING(const char *s)
{
	return NULL != s ? s : "";
}

/**
 * Is string NULL or empty?
 */
static inline gboolean
is_null_or_empty(const char *s)
{
	return NULL == s || '\0' == *s;
}

/**
 * Swap endianness of a guint32.
 *
 * @param i the guint32 to swap
 *
 * @returns the value of i after swapping its byte order.
 */
static inline G_GNUC_CONST guint32
swap_guint32(guint32 i)
{
	guint32 a;
	guint32 b;
                                  /* i -> ABCD */
	a = (i & 0x00ff00ff) << 8;    /* a -> B0D0 */
	b = (i & 0xff00ff00) >> 8;    /* b -> 0A0C */
	i = a | b;                    /* i -> BADC */
	i = (i << 16) | (i >> 16);    /* i -> DCBA */
    
	return i;
}

/**
 * Converts the given IPv4 netmask in host byte order to a CIDR prefix length.
 * No checks are performed whether the netmask is proper and if it's not
 * the result is unspecified.
 *
 * @param netmask an IPv4 netmask in host byte order.
 * @return The CIDR prefix length (0..32).
 */
static inline G_GNUC_CONST WARN_UNUSED_RESULT guint8
netmask_to_cidr(guint32 netmask)
#ifdef HAVE_BUILTIN_POPCOUNT
{
	__builtin_popcount(netmask);
}
#else	/* HAVE_BUILTIN_POPCOUNT */
{
	guint8 bits = 32;

	while (0 == (netmask & 0x1)) {
		netmask >>= 1;
		bits--;
	}
	return bits;
}
#endif /* HAVE_BUILTIN_POPCOUNT */

/**
 * Converts the CIDR prefix length to a IPv4 netmask in host byte order.
 * No checks are performed.
 *
 * @param bits A value between 1..32.
 * @return The equivalent netmask in host byte order.
 */
static inline G_GNUC_CONST WARN_UNUSED_RESULT guint32
cidr_to_netmask(guint bits)
{
	return (guint32)-1 << (32 - bits);
}

/**
 * Rounds ``n'' up so that it matches the given alignment ``align''.
 */
static inline size_t
round_size(size_t align, size_t n)
{
	size_t m = n % align;
	return m ? n + (align - m) : n;
}

/**
 * Rounds ``n'' up so that it matches the given alignment ``align''.
 * Fast version, when ``align'' is known to be a power of 2.
 */
static inline size_t
round_size_fast(size_t align, size_t n)
{
	size_t mask = align - 1;

	return (n + mask) & ~mask;
}

void guid_random_fill(struct guid *);

/*
 * Syscall wrappers for errno == 0 bug. --RAM, 27/10/2003
 */

struct stat;

static inline gboolean
is_temporary_error(int error)
{
  switch (error) {
#ifdef WSAEWOULDBLOCK
  case WSAEWOULDBLOCK:
#endif
  case EAGAIN:
#if defined(EWOULDBLOCK) && EAGAIN != EWOULDBLOCK
  case EWOULDBLOCK:
#endif /* EWOULDBLOCK != EAGAIN */
  case EINTR:
    return TRUE;
  }
  return FALSE;
}

/* Wrapper around lseek() to handle filesize -> off_t conversion. */
int seek_to_filepos(int fd, filesize_t pos);
filesize_t get_random_file_offset(const filesize_t size);

guint filesize_per_100(filesize_t size, filesize_t part);
guint filesize_per_1000(filesize_t size, filesize_t part);
guint filesize_per_10000(filesize_t size, filesize_t part);

/*
 * CIDR split of IP range.
 */

typedef void (*cidr_split_t)(guint32 ip, guint bits, gpointer udata);

void ip_range_split(
	guint32 lower_ip, guint32 upper_ip, cidr_split_t cb, gpointer udata);

/**
 * Perform a binary search over an array.
 *
 * bs_type is the type of bs_item
 * bs_key is the key to lookup
 * bs_size is the array length
 * bs_cmp(bs_item, bs_key) is used to compare the key with the current item
 * bs_get_key(bs_index) must return the key at bs_index
 * bs_found(bs_index) is executed if bs_key is found
 *
 * All local variables are prefixed with bs_ to prevent clashes with
 * other visible variables.
 */
#define BINARY_SEARCH(bs_type, bs_key, bs_size, bs_cmp, bs_get_key, bs_found) \
G_STMT_START { \
	size_t bs_index, bs_j = 0, bs_k; \
	for (bs_k = (bs_size); bs_k != 0; bs_k >>= 1) { \
		bs_type bs_item; \
		int bs_cmp_result; \
\
		bs_index = bs_j + (bs_k >> 1); \
		bs_item = bs_get_key(bs_index); \
		bs_cmp_result = bs_cmp(bs_item, bs_key); \
		if (0 == bs_cmp_result) {	\
			bs_found(bs_index); \
			break; \
		} else if (bs_cmp_result < 0) { \
			bs_j = bs_index + 1; \
			bs_k--; \
		} \
	} \
} G_STMT_END

/**
 * Ensure a table used for binary search is sorted.
 *
 * bs_array is the (static) array to scan.
 * bs_type is the type of bs_item
 * bs_field is the field in the bs_item structure to compare.
 * bs_cmp() is the comparison function to use between items
 * bs_field2str is how one can stringify the bs_field.
 *
 * Skip the first to have a previous element, tables with a single
 * element are sorted anyway.
 */
#define BINARY_ARRAY_SORTED(bs_array, bs_type, bs_field, bs_cmp, bs_field2str) \
G_STMT_START { \
	size_t bs_index; \
	size_t bs_size = G_N_ELEMENTS(bs_array); \
\
	for (bs_index = 1; bs_index < bs_size; bs_index++) { \
		const bs_type *prev = &bs_array[bs_index - 1]; \
		const bs_type *e = &bs_array[bs_index]; \
\
		if (bs_cmp(prev->bs_field, e->bs_field) >= 0) \
			g_error(STRINGIFY(bs_array) "[] unsorted (near item \"%s\")", \
				bs_field2str(e->bs_field)); \
	} \
} G_STMT_END

/**
 * Converts an integer to a single decimal ASCII digit. The are no checks,
 * this is just a convenience function.
 *
 * @param x An integer between 0 and 9.
 * @return The ASCII character corresponding to the decimal digit [0-9].
 */
static inline guchar
dec_digit(guchar x)
{
	static const char dec_alphabet[] = "0123456789";
	return dec_alphabet[x % 10];
}

/**
 * Copies "src_len" chars from "src" to "dst" reversing their order.
 * The resulting string is always NUL-terminated unless "size" is zero.
 * If "size" is not larger than "src_len", the resulting string will
 * be truncated. NUL chars copied from "src" are not treated as string
 * terminations.
 *
 * @param dst The destination buffer.
 * @param size The size of the destination buffer.
 * @param src The source buffer.
 * @param src_len The size of the source buffer.
 *
 * @return The resulting length of string not counting the termating NUL.
 *         Note that NULs that might have been copied from "src" are
 *         included in this count. Thus strlen(dst) would return a lower
 *         value in this case. 
 */
static inline size_t
reverse_strlcpy(char * const dst, size_t size,
	const char *src, size_t src_len)
{
	char *p = dst;
	
	if (size-- > 0) {
		const char *q = &src[src_len], *end = &dst[MIN(src_len, size)];

		while (p != end) {
			*p++ = *--q;
		}
		*p = '\0';
	}

	return p - dst;
}

#endif /* _misc_h_ */

/* vi: set ts=4 sw=4 cindent: */
