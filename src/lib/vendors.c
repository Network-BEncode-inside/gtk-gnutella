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

/**
 * @ingroup lib
 * @file
 *
 * Vendor code management.
 *
 * @author Richard Eckart
 * @date 2001-2003
 */

#include "common.h"

RCSID("$Id$");

#include "vendors.h"
#include "endian.h"
#include "misc.h"
#include "override.h"	/* Must be the last header included */

struct vendor {
    guint32 code;
    gchar *name;
} vendor_map[] = {
	/* This array MUST be sorted, because it is searched dichotomically */

    { T_ACQL, "AcqLite" },
    { T_ACQX, "Acquisition" },
    { T_AGNT, "Agentella" },
    { T_ARES, "Ares" },
    { T_ATOM, "AtomWire" },
    { T_AZOO, "AyZoo" },
    { T_BARE, "BearShare-v4" },
    { T_BEAR, "BearShare" },
    { T_BGNU, "brandGNU" },
    { T_COCO, "CocoGnut" },
    { T_CULT, "Cultiv8r" },
    { T_DRIP, "Driptella" },
    { T_EVIL, "Suicide" },
    { T_FEVR, "FileFever" },
    { T_FIRE, "FireFly" },
    { T_FISH, "PEERanha" },
    { T_FZZN, "Fuzzon" },
    { T_GDNA, "Gnucleus DNA" },
    { T_GIFT, "giFT" },
    { T_GNEW, "Gnewtellium" },
    { T_GNOT, "Gnotella" },
    { T_GNTD, "Gnet Daemon" },
    { T_GNTG, "Gnutelligentsia" },
    { T_GNUC, "Gnucleus" },
    { T_GNUM, "Gnuminous" },
    { T_GNUT, "Gnut" },
    { T_GTKG, "gtk-gnutella" },
    { T_HSLG, "Hagelslag" },
    { T_HUIT, "Huitella" },
    { T_JHOP, "J-Hop" },
    { T_JOEY, "Jotella" },
    { T_KIKI, "KikiTella" },
    { T_KISS, "Kisstella" },
    { T_LIME, "LimeWire" },
    { T_LION, "LionShare" },
    { T_MACT, "Mactella" },
    { T_MESH, "iMesh" },
    { T_MIRT, "Mirtella" },
    { T_MLDK, "MLDonkey" },
    { T_MMMM, "Morpheus-v2" },
    { T_MNAP, "MyNapster" },
    { T_MRPH, "Morpheus" },
    { T_MUTE, "Mutella" },
    { T_NAPS, "NapShare" },
    { T_NGET, "Gnuget" },
    { T_NOOG, "Noogtella" },
    { T_NOVA, "NovaP2P" },
    { T_OCFG, "OpenCola" },
    { T_OPRA, "Opera" },
    { T_OXID, "Oxide" },
    { T_PCST, "Peercast" },
    { T_PHEX, "Phex" },
    { T_PWRT, "PowerTella" },
    { T_QTEL, "Qtella" },
    { T_RASP, "Rasputin" },
    { T_RAZA, "Shareaza" },
    { T_SHNB, "Shinobu" },
    { T_SNOW, "FrostWire" },
    { T_SNUT, "SwapNut" },
    { T_STRM, "Storm" },
    { T_SWAP, "Swapper" },
    { T_SWFT, "Swift" },
    { T_TFLS, "TrustyFiles" },
    { T_TOAD, "ToadNode" },
    { T_VPUT, "Vputella" },
    { T_WAST, "Waste" },
    { T_XOLO, "Xolox" },
    { T_XTLA, "Xtella" },
    { T_YAFS, "UlfsYAFS" },
    { T_ZIGA, "Ziga" },
    { T_peer, "Peeranha" },

	/* Above line intentionally left blank (for "!}sort" on vi) */
};

/**
 * Find vendor name, given vendor code.
 *
 * @returns vendor string if found, NULL otherwise.
 */
static gchar *
find_vendor(guint32 code)
{
#define GET_KEY(i) (vendor_map[(i)].code)
#define FOUND(i) G_STMT_START { \
	return vendor_map[(i)].name; \
	/* NOTREACHED */ \
} G_STMT_END

	BINARY_SEARCH(guint32, code, G_N_ELEMENTS(vendor_map), VENDOR_CODE_CMP,
		GET_KEY, FOUND);

#undef FOUND
#undef GET_KEY
	return NULL; /* not found */
}

/**
 * @return true is gtk-gnutella knows the given 4-byte vendor code.
 */
gboolean
is_vendor_known(union vendor_code code)
{
    if (code.be32 == 0)
        return FALSE;

	return find_vendor(ntohl(code.be32)) != NULL;
}

/**
 * Make up a printable version of the vendor code.
 *
 * @param code A 4-letter Gnutella vendor ID in host-endian order thus
 *        after peek_be32() or ntohl().
 *
 * @return pointer to static data.
 */
const gchar *
vendor_code_str(guint32 code)
{
	static gchar temp[1 + sizeof code];
    guint i;

	STATIC_ASSERT(5 == G_N_ELEMENTS(temp));

    if (code == 0)
		return "null";

	poke_be32(&temp[0], code);
	for (i = 0; i < G_N_ELEMENTS(temp) - 1; i++) {
		if (!is_ascii_print(temp[i]))
			temp[i] = '.';
	}

	temp[4] = '\0';

	return temp;
}

/**
 * Return the "human readable" name associated with the 4-byte vendor code.
 * If we can't understand the code return NULL or if the 4-byte code
 * consists only of printable characters, return the code as a string.
 */
const gchar *
lookup_vendor_name(union vendor_code code)
{
	static gchar temp[1 + G_N_ELEMENTS(code.b)];
	gchar *name;
    guint i;

	STATIC_ASSERT(5 == G_N_ELEMENTS(temp));
	
    if (code.be32 == 0)
        return NULL;

	name = find_vendor(ntohl(code.be32));
	if (name != NULL)
		return name;

	
	/* Unknown type, look whether we have all printable ASCII */
	for (i = 0; i < G_N_ELEMENTS(code.b); i++) {
		if (is_ascii_print(code.b[i]))
            temp[i] = code.b[i];
		else {
            temp[0] = '\0';
			break;
		}
	}
	temp[4] = '\0';

	return temp[0] ? temp : NULL;
}

/**
 * Initialize the vendor lookup.
 */
void
vendor_init(void)
{
	BINARY_ARRAY_SORTED(vendor_map, struct vendor, code,
		VENDOR_CODE_CMP, vendor_code_str);
}

/* vi: set ts=4 sw=4 cindent: */
