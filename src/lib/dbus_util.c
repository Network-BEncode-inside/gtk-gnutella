/*
 * $Id$
 *
 * Copyright (c) 2005, Hans de Graaff
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
 * Interface to dbus messaging bus.
 *
 * gtk-gnutella will send notifications on the dbus message bus as
 * signals with a string parameter. Depending on the signal the
 * parameter will have a different meaning. Also see the documenation
 * in doc/other/dbus-support.txt
 *
 * @author Hans de Graaff
 * @date 2005
 */

#include "common.h"

RCSID("$Id$")

#include "dbus_util.h"
#include "misc.h"			/* For str_chomp() */

#ifdef HAS_DBUS

/** @todo DBus API is not stable yet, may need changes once 1.0 is released */
#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus.h>

/** The dbus path to the object serving the notifications. */
#define DBUS_PATH "/net/gtkg/events"
/** The interface that is sending the notifications. */
#define DBUS_INTERFACE "net.gtkg.Events"

static DBusConnection *bus = NULL; /**< D-Bus connection to the message bus */

/**
 * Initialize the bus connection.
 */
void
dbus_util_init(void)
{
	DBusError error;

	dbus_error_init(&error);
	bus = dbus_bus_get(DBUS_BUS_SESSION, &error);

	if (NULL == bus) {
		str_chomp(error.message, 0);
		g_message("could not open connection to DBus bus: %s", error.message);
		dbus_error_free(&error);
	} else {

		g_message("D-BUS set up and ready for use.");
		/** @todo Include a timestamp or some other useful info */
		dbus_util_send_message(DBS_EVT, "started");
	}
}

/**
 * Close down the D-BUS connection and send final event.
 */
void
dbus_util_close(void)
{
	/** @todo Include a timestamp or some other useful info */
	dbus_util_send_message(DBS_EVT, "stopped");

	/**
	 * @todo It's not really clear to me how I can free the bus that
	 * we have, but since we are shutting down now anyway it does not
	 * matter much except for the spotless record of memory
	 * reclaiming.
	*/
}


/**
 * Send a message on the bus.
 * @param signal_name The name of the dbus signal to use
 * @param text The text to append to the message, NULL if n/a
 */
void
dbus_util_send_message(const char *signal_name, const char *text)
{
	DBusMessage *message;  /**< The dbus message to send */

	/*
	 * If the bus could not be initialized earlier then we should not
	 * attempt to send a message now.
	 */
	if (NULL == bus)
		return;

	/* Create a new message on the DBUS_INTERFACE */
	message = dbus_message_new_signal(DBUS_PATH, DBUS_INTERFACE, signal_name);

	if (NULL == message) {
		g_message("could not create D-BUS message!");
	} else {

		/* Add the message to the Events signal */
		dbus_message_append_args(message, DBUS_TYPE_STRING, &text,
								 DBUS_TYPE_INVALID);

		/* Send the message */
		dbus_connection_send(bus, message, NULL);

#if 0
		g_message("Sent D-BUS signal '%s': %s", signal_name, text);
#endif

		/* Free the message */
		dbus_message_unref(message);
    }
}

#endif /* HAS_DBUS */

/* vi: set ts=4 sw=4 cindent: */
