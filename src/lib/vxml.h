/*
 * $Id$
 *
 * Copyright (c) 2010, Raphael Manfredi
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
 * Versatile XML processing.
 *
 * @author Raphael Manfredi
 * @date 2010
 */

#ifndef _vxml_h_
#define _vxml_h_

#include "common.h"
#include "nv.h"

struct vxml_parser;
typedef struct vxml_parser vxml_parser_t;

/**
 * Parsing options.
 */
#define VXML_O_STRIP_NS			(1 << 0)  /**< Strip namespace from elements */
#define VXML_O_STRICT_COMMENTS	(1 << 1)  /**< Disable '--' in comments */
#define VXML_O_FATAL			(1 << 2)  /**< Abort on fatal error */
#define VXML_O_STRIP_BLANKS		(1 << 3)  /**< Strip leading/ending blanks */

/**
 * Error codes.
 */
typedef enum {
	VXML_E_OK = 0,						/**< No error */
	VXML_E_UNSUPPORTED_BYTE_ORDER,		/**< Unsupported byte order */
	VXML_E_UNSUPPORTED_CHARSET,			/**< Unsupported character set */
	VXML_E_TRUNCATED_INPUT,				/**< Truncated input stream */
	VXML_E_EXPECTED_NAME_START,			/**< Expected a valid name start */
	VXML_E_INVALID_CHAR_REF,			/**< Invalid character reference */
	VXML_E_INVALID_CHARACTER,			/**< Invalid Unicode character */
	VXML_E_INVALID_NAME_CHARACTER,		/**< Invalid character in name */
	VXML_E_UNKNOWN_ENTITY_REF,			/**< Unknown entity reference */
	VXML_E_UNEXPECTED_CHARACTER,		/**< Unexpected character */
	VXML_E_UNEXPECTED_WHITESPACE,		/**< Unexpected white space */
	VXML_E_BAD_CHAR_IN_NAME,			/**< Bad character in name */
	VXML_E_INVALID_TAG_NESTING,			/**< Invalid tag nesting */
	VXML_E_EXPECTED_QUOTE,				/**< Was expecting a quote */
	VXML_E_EXPECTED_GT,					/**< Was expecting a '>' */
	VXML_E_EXPECTED_SPACE,				/**< Was expecting a space */
	VXML_E_EXPECTED_LBRAK,				/**< Was expecting a '[' */
	VXML_E_EXPECTED_RBRAK,				/**< Was expecting a ']' */
	VXML_E_EXPECTED_TWO_MINUS,			/**< Was expecting '--' */
	VXML_E_EXPECTED_DECL_TOKEN,			/**< Was expecting declaration token */
	VXML_E_EXPECTED_NDATA_TOKEN,		/**< Was expecting 'NDATA' token */
	VXML_E_EXPECTED_CDATA_TOKEN,		/**< Was expecting 'CDATA' token */
	VXML_E_EXPECTED_COND_TOKEN,			/**< Expected 'INCLUDE' or 'IGNORE' */
	VXML_E_EXPECTED_DOCTYPE_DECL,		/**< Was expecting DOCTYPE decl. */
	VXML_E_UNEXPECTED_LT,				/**< Was not expecting a '<' */
	VXML_E_UNEXPECTED_XML_PI,			/**< Spurious "<?xml ...> */
	VXML_E_NESTED_DOCTYPE_DECL,			/**< Was not expecting DOCTYPE decl. */
	VXML_E_INVALID_VERSION,				/**< Invalid version number */
	VXML_E_VERSION_OUT_OF_RANGE,		/**< Version number out of range */
	VXML_E_UNKNOWN_CHAR_ENCODING_NAME,	/**< Unknown character encoding name */
	VXML_E_INVALID_CHAR_ENCODING_NAME,	/**< Invalid character encoding name */
	VXML_E_UNREADABLE_CHAR_ENCODING,	/**< Unreadable input */
	VXML_E_USER,						/**< User-defined error */

	VXML_E_MAX
} vxml_error_t;

/**
 * Start plain element callback signature.
 *
 * @param vp		the parser
 * @param name		the element name (UTF-8 string)
 * @param attrs		element attributes (name/value pair table)
 * @param data		user-specified callback argument
 */
typedef void (*vxml_p_element_start_cb_t)(vxml_parser_t *vp,
	const char *name, const nv_table_t *attrs, void *data);

/**
 * Start tokenized element callback signature.
 *
 * @param vp		the parser
 * @param id		the element token ID (user-defined)
 * @param attrs		element attributes (name/value pair table)
 * @param data		user-specified callback argument
 */
typedef void (*vxml_t_element_start_cb_t)(vxml_parser_t *vp,
	unsigned id, const nv_table_t *attrs, void *data);

/**
 * Plain element text callback signature.
 *
 * @param vp		the parser
 * @param name		the element name (UTF-8 string)
 * @param text		the text data (NUL-terminated, in UTF-8)
 * @param len		length of text data
 * @param data		user-specified callback argument
 */
typedef void (*vxml_p_text_cb_t)(vxml_parser_t *vp,
	const char *name, const char *text, size_t len, void *data);

/**
 * Tokenized element text callback signature.
 *
 * @param vp		the parser
 * @param id		the element token ID (user-defined)
 * @param text		the text data (NUL-terminated, in UTF-8)
 * @param len		length of text data
 * @param data		user-specified callback argument
 */
typedef void (*vxml_t_text_cb_t)(vxml_parser_t *vp,
	unsigned id, const char *text, size_t len, void *data);

/**
 * End plain element callback signature.
 *
 * @param vp		the parser
 * @param name		the element name (UTF-8 string)
 * @param data		user-specified callback argument
 */
typedef void (*vxml_p_element_end_cb_t)(vxml_parser_t *vp,
	const char *name, void *data);

/**
 * End tokenized element callback signature.
 *
 * @param vp		the parser
 * @param id		the element token ID (user-defined)
 * @param data		user-specified callback argument
 */
typedef void (*vxml_t_element_end_cb_t)(vxml_parser_t *vp,
	unsigned id, void *data);

/**
 * Regroups the parsing callbacks on elements.
 *
 * Any callback can be specified as NULL in which case it will not be
 * invoked.
 *
 * When both a tokenized and a non-tokenized (plain) element callback are
 * defined, the tokenized one is invoked if the parser is able to tokenize
 * the element.
 */
struct vxml_ops {
	vxml_p_element_start_cb_t plain_start;
	vxml_p_text_cb_t plain_text;
	vxml_p_element_end_cb_t plain_end;
	vxml_t_element_start_cb_t tokenized_start;
	vxml_t_text_cb_t tokenized_text;
	vxml_t_element_end_cb_t tokenized_end;
};

/**
 * A tokenized element.
 */
struct vxml_token {
	const char *name;		/**< Element name (UTF-8) */
	unsigned id;			/**< Corresponding token */
};

/**
 * A parsing token description, the mapping between a name and a number.
 */
struct vxml_parser_token {
	const char *name;
	unsigned value;
};

/*
 * Public interface.
 */

void vxml_test(void);
void set_vxml_debug(guint32 level);
gboolean vxml_debugging(guint32 level);

const char *vxml_strerror(vxml_error_t error);
const char *vxml_parser_strerror(const vxml_parser_t *vp, vxml_error_t error);
vxml_parser_t *vxml_parser_make(const char *name, guint32 options);
void vxml_parser_free(vxml_parser_t *vp);
void vxml_parser_add_input(vxml_parser_t *vp, const char *data, size_t length);
void vxml_parser_set_tokens(vxml_parser_t *vp,
	struct vxml_token *tvec, size_t tlen);
vxml_error_t vxml_parse(vxml_parser_t *vp);
vxml_error_t vxml_parse_callbacks(vxml_parser_t *vp,
	const struct vxml_ops *ops, void *data);
vxml_error_t vxml_parse_callbacks_tokens(vxml_parser_t *vp,
	const struct vxml_ops *ops,
	struct vxml_token *tvec, size_t tlen, void *data);

unsigned vxml_token_lookup(const char *name,
	const struct vxml_parser_token *tokens, size_t len);

void vxml_parser_error(vxml_parser_t *vp, const char *errstr, ...);
unsigned vxml_parser_depth(const vxml_parser_t *vp);
const char *vxml_parser_current_element(const vxml_parser_t *vp);
const char *vxml_parser_parent_element(const vxml_parser_t *vp);
const char *vxml_parser_nth_parent_element(const vxml_parser_t *vp, size_t n);

#endif /* _vxml_h_ */

/* vi: set ts=4 sw=4 cindent: */
