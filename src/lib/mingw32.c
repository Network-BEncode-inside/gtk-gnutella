/*
 * Copyright (c) 2010 Jeroen Asselman & Raphael Manfredi
 * Copyright (c) 2012 Raphael Manfredi
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
 * Win32 cross-compiling utility routines.
 *
 * @author Jeroen Asselman
 * @date 2010
 * @author Raphael Manfredi
 * @date 2010-2012
 */

#include "common.h"

/*
 * This whole file is only compiled under Windows.
 */

#ifdef MINGW32

#include <stdlib.h>
#include <windows.h>
#include <mswsock.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <psapi.h>
#include <winnt.h>
#include <powrprof.h>
#include <conio.h>				/* For _kbhit() */
#include <imagehlp.h>			/* For backtrace() emulation */
#include <iphlpapi.h>			/* For GetBestRoute() */
#include <bfd.h>				/* For access to debugging information */

#include <glib.h>
#include <glib/gprintf.h>

#include <stdio.h>
#include <wchar.h>

#include "host_addr.h"			/* ADNS */
#include "adns.h"
#include "adns_msg.h"

#include "ascii.h"				/* For is_ascii_alpha() */
#include "bfd_util.h"
#include "constants.h"
#include "cq.h"
#include "crash.h"
#include "debug.h"
#include "dl_util.h"
#include "endian.h"
#include "fd.h"					/* For is_open_fd() */
#include "glib-missing.h"
#include "halloc.h"
#include "hset.h"
#include "iovec.h"
#include "log.h"
#include "mem.h"
#include "mempcpy.h"
#include "misc.h"
#include "path.h"				/* For filepath_basename() */
#include "product.h"
#include "stacktrace.h"
#include "str.h"
#include "stringify.h"			/* For ULONG_DEC_BUFLEN */
#include "unsigned.h"
#include "utf8.h"
#include "walloc.h"
#include "xmalloc.h"

#include "override.h"			/* Must be the last header included */

#if 0
#define MINGW_SYSCALL_DEBUG		/**< Trace all Windows API call errors */
#endif
#if 0
#define MINGW_STARTUP_DEBUG		/**< Trace early startup stages */
#endif
#if 0
#define MINGW_BACKTRACE_DEBUG	/**< Trace our own backtracing */
#endif

#undef signal

#undef stat
#undef fstat
#undef open
#undef fopen
#undef freopen
#undef read
#undef write
#undef mkdir
#undef access
#undef chdir
#undef remove
#undef lseek
#undef dup2
#undef unlink
#undef opendir
#undef readdir
#undef closedir

#undef getaddrinfo
#undef freeaddrinfo

#undef select
#undef socket
#undef bind
#undef connect
#undef listen
#undef accept
#undef shutdown
#undef getsockopt
#undef setsockopt
#undef recv
#undef sendto

#undef abort

#define VMM_MINSIZE		(1024*1024*100)	/* At least 100 MiB */
#define WS2_LIBRARY		"ws2_32.dll"

/* Offset of the UNIX Epoch compared to the Window's one, in microseconds */
#define EPOCH_OFFSET	UINT64_CONST(11644473600000000)

#ifdef MINGW_SYSCALL_DEBUG
#define mingw_syscall_debug()	1
#else
#define mingw_syscall_debug()	0
#endif

static HINSTANCE libws2_32;
static bool mingw_inited;

typedef struct processor_power_information {
  ULONG Number;
  ULONG MaxMhz;
  ULONG CurrentMhz;
  ULONG MhzLimit;
  ULONG MaxIdleState;
  ULONG CurrentIdleState;
} PROCESSOR_POWER_INFORMATION;

extern bool vmm_is_debugging(uint32 level);

typedef int (*WSAPoll_func_t)(WSAPOLLFD fdarray[], ULONG nfds, INT timeout);
WSAPoll_func_t WSAPoll = NULL;

/**
 * Path Name Conversion Structure.
 *
 * @note MAX_PATH_LEN might actually apply to MBCS only and the limit for
 * Unicode is 32768. So we could (or should?) support pathnames longer than
 * 256 characters.
 */
typedef struct pncs {
	wchar_t *utf16;
	wchar_t buf[MAX_PATH_LEN];
} pncs_t;

/**
 * Converts a NUL-terminated MBCS string to an UTF-16 string.
 * @note mbtowcs() is not async-signal safe.
 *
 * @param src The string to convert.
 * @param dest The destination buffer.
 * @param dest_size The size of the destination buffer in number of elements.
 *
 * @return NULL on failure with errno set, dest on success.
 */
static wchar_t *
locale_to_wchar(const char *src, wchar_t *dest, size_t dest_size)
{
	size_t n;

	n = mbstowcs(NULL, src, 0);
	if ((size_t) -1 == n)
		return NULL;

	if (n < dest_size) {
		(void) mbstowcs(dest, src, dest_size);
	} else {
		dest = NULL;
		errno = ENAMETOOLONG;
	}
	return dest;
}

/*
 * Build a native path for the underlying OS.
 *
 * When launched from a Cygwin or MinGW environment, we can face
 * paths like "/x/file" which really mean "x:/file" in Windows parlance.
 * Moreover, in Cygwin, unless configured otherwise, Windows paths are
 * prefixed with "/cygdrive/", so "x:/file" would be "/cygdrive/x/file";
 *
 * Since we're going to issue a Windows call, we need to translate
 * these paths so that Windows can locate the file properly.
 *
 * @attention
 * If they create a C:/x directory, when /x/path could be "c:/x/path" and
 * we will wrongly interpret is as X:/path.  The chance they create
 * single-letter top-level directories is small in practice.
 *
 * @return pointer to static data containing the "native" path or NULL on
 * error, with the error code returned in ``error''.
 */
static const char *
get_native_path(const char *pathname, int *error)
{
	static char pathname_buf[MAX_PATH_LEN];
	const char *npath = pathname;
	char *p;

	/*
	 * Skip leading "/cygdrive/" string, up to the second "/".
	 *
	 * We can't really check whether we're running on Cygwin at run-time.
	 * Moreover, users can say "mount -c /cyg" to change the prefix from
	 * the default, so "/cygdrive/" is only a wild guess that will work
	 * with default Cygwin settings or when users say "mount -c /" to
	 * suppress the prefixing, in which case paths will look as they do on
	 * the MinGW environment.
	 */

	p = is_strcaseprefix(npath, "/cygdrive/");
	if (NULL != p)
		npath = p - 1;			/* Go back to ending "/" */

	/*
	 * Replace /x/file with x:/file.
	 *
	 * We could check that "/x" does not exist before doing this conversion,
	 * but what if we're on drive X: and there is a "X:/x" file there?
	 * Would /x/x/file be referring to X:/x/file?  What if /x/x exists?
	 *
	 * Since there is no easy way to avoid mistakes, let's keep the mangling
	 * algorithm straightforward so that error cases are also known and
	 * predictable enough.
	 */

	if (
		is_dir_separator(npath[0]) &&
		is_ascii_alpha(npath[1]) &&
		(is_dir_separator(npath[2]) || '\0' == npath[2])
	) {
		size_t plen = strlen(npath);

		if (sizeof pathname_buf <= plen) {
			*error = ENAMETOOLONG;
			return NULL;
		}

		clamp_strncpy(pathname_buf, sizeof pathname_buf, npath, plen);
		pathname_buf[0] = npath[1]; /* Replace with correct drive letter */
		pathname_buf[1] = ':';
		npath = pathname_buf;
	}

	return npath;
}

/**
 * @return native path corresponding to the given path, as pointer to
 * static data.
 */
const char *
mingw_native_path(const char *pathname)
{
	const char *npath;		/* Native path */
	int error;

	npath = get_native_path(pathname, &error);

	return NULL == npath ? pathname : npath;
}

/**
 * Convert pathname to a UTF-16 representation.
 *
 * On success, the member utf16 points to the converted pathname that can be
 * used in Unicode-aware Windows calls.
 *
 * @return 0 on success, -1 on error with errno set.
 */
static int
pncs_convert(pncs_t *pncs, const char *pathname)
{
	const char *npath;		/* Native path */
	int error;

	/* On Windows wchar_t should always be 16-bit and use UTF-16 encoding. */
	STATIC_ASSERT(sizeof(uint16) == sizeof(wchar_t));

	if (NULL == (npath = get_native_path(pathname, &error))) {
		errno = error;
		return -1;
	}

	if (utf8_is_valid_string(npath)) {
		size_t ret;

		ret = utf8_to_utf16(npath, pncs->buf, G_N_ELEMENTS(pncs->buf));
		if (ret < G_N_ELEMENTS(pncs->buf)) {
			pncs->utf16 = pncs->buf;
		} else {
			errno = ENAMETOOLONG;
			pncs->utf16 = NULL;
		}
	} else {
		pncs->utf16 =
			locale_to_wchar(npath, pncs->buf, G_N_ELEMENTS(pncs->buf));
	}

	return NULL != pncs->utf16 ? 0 : -1;
}

static inline bool
mingw_fd_is_opened(int fd)
{
	unsigned long dummy;

	return (HANDLE) _get_osfhandle(fd) != INVALID_HANDLE_VALUE ||
		 0 == WSAHtonl((SOCKET) fd, 666, &dummy);
}

/**
 * Get last Winsock operation error code, remapping Winsocks-specific
 * errors into POSIX ones, which upper level code expects.
 */
static int
mingw_wsa_last_error(void)
{
	int error = WSAGetLastError();
	int result = error;

	/*
	 * Not all the Winsock error codes are translated here.  The ones
	 * not conflicting with other POSIX defines have already been mapped.
	 *
	 * For instance, we have:
	 *
	 *     #define ENOTSOCK WSAENOTSOCK
	 *
	 * so there is no need to catch WSAENOTSOCK here to translate it to
	 * ENOTSOCK since the remapping has already been done for that constant.
	 *
	 * So the ones that remain are those which are not really socket-specific
	 * and for which we cannot do a remapping that would conflict with other
	 * definitions in the MinGW headers.
	 */

	switch (error) {
	case WSAEWOULDBLOCK:	result = EAGAIN; break;
	case WSAEINTR:			result = EINTR; break;
	case WSAEINVAL:			result = EINVAL; break;
	}

	if (mingw_syscall_debug()) {
		s_debug("%s() failed: %s (%d)", stacktrace_caller_name(1),
			symbolic_errno(result), error);
	}

	return result;
}

/**
 * Remap Windows-specific errors into POSIX ones, also clearing the POSIX
 * range so that strerror() works.
 */
static int
mingw_win2posix(int error)
{
	static hset_t *warned;

	if (NULL == warned) {
		warned = NOT_LEAKING(hset_create(HASH_KEY_SELF, 0));
	}

	/*
	 * This is required when using non-POSIX routines, for instance
	 * _wmkdir() instead of mkdir(), so that regular errno procesing
	 * can occur in the code.
	 *
	 * MinGW also defines POSIX error codes up to 42, but they are
	 * conflicting with Windows error ones, so these must also be remapped.
	 *
	 * FIXME: Many errors are missing, only the first ones are handled.
	 * A warning will be emitted when we hit an un-remapped error, but this
	 * is going to be a painful iterative convergence.
	 */

	switch (error) {
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		return EEXIST;
	case ERROR_INVALID_FUNCTION:
		return ENOSYS;
	case ERROR_FILE_NOT_FOUND:
		return ENOFILE;
	case ERROR_PATH_NOT_FOUND:
		return ENOENT;
	case ERROR_TOO_MANY_OPEN_FILES:
		return EMFILE;
	case ERROR_INVALID_HANDLE:
		return EBADF;
	case ERROR_NOT_ENOUGH_MEMORY:
		return ENOMEM;
	case ERROR_ACCESS_DENIED:
	case ERROR_INVALID_ACCESS:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return EACCES;
	case ERROR_OUTOFMEMORY:
		return ENOMEM;
	case ERROR_INVALID_DRIVE:
		return ENXIO;
	case ERROR_NOT_SAME_DEVICE:
		return EXDEV;
	case ERROR_NO_MORE_FILES:
		return ENFILE;
	case ERROR_WRITE_PROTECT:
		return EPERM;
	case ERROR_NOT_SUPPORTED:
		return ENOSYS;
	case ERROR_DISK_FULL:
		return ENOSPC;
	case ERROR_BROKEN_PIPE:
		return EPIPE;
	case ERROR_INVALID_NAME:		/* Invalid syntax in filename */
	case ERROR_INVALID_PARAMETER:	/* Invalid function parameter */
		return EINVAL;
	case ERROR_DIRECTORY:			/* "Directory name is invalid" */
		return ENOTDIR;				/* Seems the closest mapping */
	case WSAENOTSOCK:				/* For fstat() calls */
		return ENOTSOCK;
	/*
	 * The following remapped because their number is in the POSIX range
	 */
	case ERROR_ARENA_TRASHED:
		return EFAULT;
	case ERROR_INVALID_BLOCK:
		return EIO;
	case ERROR_BAD_ENVIRONMENT:
		return EFAULT;
	case ERROR_BAD_FORMAT:
		return EINVAL;
	case ERROR_INVALID_DATA:
		return EIO;
	case ERROR_CURRENT_DIRECTORY:
		return ENOFILE;
	case ERROR_BAD_UNIT:
	case ERROR_BAD_DEVICE:
		return ENODEV;
	case ERROR_NOT_READY:
	case ERROR_BAD_COMMAND:
	case ERROR_CRC:
	case ERROR_BAD_LENGTH:
	case ERROR_SEEK:
	case ERROR_NOT_DOS_DISK:
	case ERROR_SECTOR_NOT_FOUND:
		return EIO;
	case ERROR_OUT_OF_PAPER:
		return ENOSPC;
	case ERROR_WRITE_FAULT:
	case ERROR_READ_FAULT:
	case ERROR_NOACCESS:		/* Invalid access to memory location */
		return EFAULT;
	case ERROR_GEN_FAILURE:
	case ERROR_WRONG_DISK:
	case ERROR_SHARING_BUFFER_EXCEEDED:
		return EIO;
	case ERROR_HANDLE_EOF:
		return 0;			/* EOF must be treated as a read of 0 bytes */
	case ERROR_HANDLE_DISK_FULL:
		return ENOSPC;
	case ERROR_ENVVAR_NOT_FOUND:
		/* Got this error writing to a closed stdio fd, opened via pipe() */
		return EBADF;
	default:
		if (!hset_contains(warned, int_to_pointer(error))) {
			s_warning("Windows error code %d (%s) not remapped to a POSIX one",
				error, g_strerror(error));
			hset_insert(warned, int_to_pointer(error));
		}
	}

	return error;
}

/**
 * Get last Windows error, remapping Windows-specific errors into POSIX ones
 * and clearing the POSIX range so that strerror() works.
 */
static int
mingw_last_error(void)
{
	int error = GetLastError();
	int result = mingw_win2posix(error);

	if (mingw_syscall_debug()) {
		s_debug("%s() failed: %s (%d)", stacktrace_caller_name(1),
			symbolic_errno(result), error);
	}

	return result;
}

static signal_handler_t mingw_sighandler[SIGNAL_COUNT];

signal_handler_t
mingw_signal(int signo, signal_handler_t handler)
{
	signal_handler_t res;

	g_assert(handler != SIG_ERR);

	if (signo <= 0 || signo >= SIGNAL_COUNT) {
		errno = EINVAL;
		return SIG_ERR;
	}

	/*
	 * Don't call signal() with SIGBUS or SIGTRAP: since we're faking them,
	 * we'll get an error back as "unrecognized argument value".
	 */

	switch (signo) {
	case SIGBUS:
	case SIGTRAP:
		res = mingw_sighandler[signo];
		mingw_sighandler[signo] = handler;
		break;
	default:
		res = signal(signo, handler);
		if (SIG_ERR != res) {
			mingw_sighandler[signo] = handler;
		}
		break;
	}

	return res;
}

/**
 * Synthesize a fatal signal as the kernel would on an exception.
 */
static G_GNUC_COLD void
mingw_sigraise(int signo)
{
	g_assert(signo > 0 && signo < SIGNAL_COUNT);

	if (SIG_IGN == mingw_sighandler[signo]) {
		/* Nothing */
	} else if (SIG_DFL == mingw_sighandler[signo]) {
		DECLARE_STR(3);

		print_str("Got uncaught ");			/* 0 */
		print_str(signal_name(signo));		/* 1 */
		print_str(" -- crashing.\n");		/* 2 */
		flush_err_str();
		if (log_stdout_is_distinct())
			flush_str(STDOUT_FILENO);
	} else {
		(*mingw_sighandler[signo])(signo);
	}
}

/**
 * Our own abort(), to avoid the message:
 *
 * "This application has requested the Runtime to terminate it in an
 * unusual way. Please contact the application's support team for more
 * information."
 */
void
mingw_abort(void)
{
	mingw_sigraise(SIGABRT);
	ExitProcess(EXIT_FAILURE);
}

int
mingw_fcntl(int fd, int cmd, ... /* arg */ )
{
	int res = -1;

	/* If fd isn't opened, _get_osfhandle() fails with errno set to EBADF */
	if (!mingw_fd_is_opened(fd)) {
		errno = EBADF;
		return -1;
	}

	switch (cmd) {
		case F_SETFL:
			res = 0;
			break;
		case F_GETFL:
			res = O_RDWR;
			break;
		case F_SETLK:
		{
			HANDLE file = (HANDLE) _get_osfhandle(fd);
			DWORD start_high, start_low;
			DWORD len_high, len_low;
			struct flock *arg;
			va_list args;

			va_start(args, cmd);
			arg = va_arg(args, struct flock *);
			va_end(args);

			if (arg->l_whence != SEEK_SET) {
				errno = EINVAL;
				return -1;		/* This emulation only supports SEEK_SET */
			}

			if (0 == arg->l_len) {
				/* Special, 0 means the whole file */
				len_high = MAX_INT_VAL(uint32);
				len_low = MAX_INT_VAL(uint32);
			} else {
				len_high = (uint64) arg->l_len >> 32;
				len_low = arg->l_len & MAX_INT_VAL(uint32);
			}
			start_high = (uint64) arg->l_start >> 32;
			start_low = arg->l_start & MAX_INT_VAL(uint32);

			if (arg->l_type == F_WRLCK) {
				if (!LockFile(file, start_low, start_high, len_low, len_high))
					errno = mingw_last_error();
				else
					res = 0;
			} else if (arg->l_type == F_UNLCK) {
				if (!UnlockFile(file, start_low, start_high, len_low, len_high))
					errno = mingw_last_error();
				else
					res = 0;
			}
			break;
		}
		case F_DUPFD:
		{
			va_list args;
			int min, max;
			int i;

			va_start(args, cmd);
			min = va_arg(args, int);
			va_end(args);

			max = getdtablesize();

			if (min < 0 || min >= max) {
				errno = EINVAL;
				return -1;
			}

			for (i = min; i < max; i++) {
				if (mingw_fd_is_opened(i))
					continue;
				return mingw_dup2(fd, i);
			}
			errno = EMFILE;
			break;
		}
		default:
			res = -1;
			errno = EINVAL;
			break;
	}

	return res;
}

/**
 * Is WSAPoll() supported?
 */
bool
mingw_has_wsapoll(void)
{
	/*
	 * Since there is no binding in MinGW for WSAPoll(), we use the dynamic
	 * linker to fetch the routine address in the library.
	 * Currently, Configure cannot statically determine whether the
	 * feature exists...
	 *		--RAM, 2010-12-14
	 */

	return WSAPoll != NULL;
}

/**
 * Drop-in replacement for poll(), provided WSAPoll() exists.
 *
 * Use mingw_has_wsapoll() to check for WSAPoll() availability at runtime.
 */
int
mingw_poll(struct pollfd *fds, unsigned int nfds, int timeout)
{
	int res;

	if (NULL == WSAPoll) {
		errno = WSAEOPNOTSUPP;
		return -1;
	}
	res = WSAPoll(fds, nfds, timeout);
	if (SOCKET_ERROR == res)
		errno = mingw_wsa_last_error();
	return res;
}

/**
 * Get special folder path as a UTF-8 string.
 *
 * @param which		which special folder to get (CSIDL code)
 * @param what		English description of ``which'', for error logging.
 *
 * @return read-only constant string.
 */
static const char *
get_special(int which, char *what)
{
	static wchar_t pathname[MAX_PATH];
	static char utf8_path[MAX_PATH];
	int ret;

	ret = SHGetFolderPathW(NULL, which, NULL, 0, pathname);

	if (E_INVALIDARG != ret) {
		size_t conv = utf16_to_utf8(pathname, utf8_path, sizeof utf8_path);
		if (conv > sizeof utf8_path) {
			s_warning("cannot convert %s path from UTF-16 to UTF-8", what);
			ret = E_INVALIDARG;
		}
	}

	if (E_INVALIDARG == ret) {
		s_carp("%s: could not get the %s directory", G_STRFUNC, what);
		/* ASCII is valid UTF-8 */
		g_strlcpy(utf8_path, G_DIR_SEPARATOR_S, sizeof utf8_path);
	}

	return mingw_inited ? constant_str(utf8_path) : utf8_path;
}

const char *
mingw_get_home_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_LOCAL_APPDATA, "home");

	return result;
}

const char *
mingw_get_personal_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_PERSONAL, "My Documents");

	return result;
}

const char *
mingw_get_common_docs_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_COMMON_DOCUMENTS, "Common Documents");

	return result;
}

const char *
mingw_get_common_appdata_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_COMMON_APPDATA, "Common Application Data");

	return result;
}

const char *
mingw_get_admin_tools_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_ADMINTOOLS, "Admin Tools");

	return result;
}

const char *
mingw_get_windows_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_WINDOWS, "Windows");

	return result;
}

const char *
mingw_get_system_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_SYSTEM, "system");

	return result;
}

const char *
mingw_get_internet_cache_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_INTERNET_CACHE, "Internet Cache");

	return result;
}

const char *
mingw_get_mypictures_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_MYPICTURES, "My Pictures");

	return result;
}

const char *
mingw_get_program_files_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_PROGRAM_FILES, "Program Files");

	return result;
}

const char *
mingw_get_fonts_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_FONTS, "Font");

	return result;
}

const char *
mingw_get_startup_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_STARTUP, "Startup");

	return result;
}

const char *
mingw_get_history_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_HISTORY, "History");

	return result;
}

const char *
mingw_get_cookies_path(void)
{
	static const char *result;

	if G_UNLIKELY(NULL == result)
		result = get_special(CSIDL_COOKIES, "Cookies");

	return result;
}

/**
 * Build path to file as "<personal_dir>\gtk-gnutella\file" without allocating
 * any memory.  If the resulting path is too long, use "/file" instead.
 * If the directory "<personal_dir>\gtk-gnutella" doest not exist yet, it
 * is created.  If the directory "<personal_dir>" does not exist, use "/file".
 *
 * @param file		name of file
 * @param dest		destination for result path
 * @param size		size of dest
 *
 * @return the address of the dest parameter.
 */
static const char *
mingw_build_personal_path(const char *file, char *dest, size_t size)
{
	const char *personal;

	/*
	 * So early in the startup process, we cannot allocate memory via the VMM
	 * layer, hence we cannot use mingw_get_personal_path() because that
	 * would cache the address of a global variable.
	 */

	personal = get_special(CSIDL_PERSONAL, "My Documents");

	g_strlcpy(dest, personal, size);

	if (path_does_not_exist(personal))
		goto fallback;

	clamp_strcat(dest, size, G_DIR_SEPARATOR_S);
	clamp_strcat(dest, size, product_get_name());

	/*
	 * Can't use mingw_mkdir() as we can't allocate memory here.
	 * Use raw mkdir() but this won't work if there are non-ASCII chars
	 * in the path.
	 */

	if (path_does_not_exist(dest))
		mkdir(dest);

	clamp_strcat(dest, size, G_DIR_SEPARATOR_S);
	clamp_strcat(dest, size, file);

	if (0 != strcmp(filepath_basename(dest), file))
		goto fallback;

	return dest;

fallback:
	g_strlcpy(dest, G_DIR_SEPARATOR_S, size);
	clamp_strcat(dest, size, file);
	return dest;
}

/**
 * Return default stdout logfile when launched from the GUI.
 * Directories leading to the dirname of the result are created as needed.
 * This routine does not allocate any memory.
 */
static const char *
mingw_getstdout_path(void)
{
	static char pathname[MAX_PATH];

	return mingw_build_personal_path("gtkg.stdout", pathname, sizeof pathname);
}

/**
 * Return default stderr logfile when launched from the GUI.
 * Directories leading to the dirname of the result are created as needed.
 * This routine does not allocate any memory.
 */
static const char *
mingw_getstderr_path(void)
{
	static char pathname[MAX_PATH];

	return mingw_build_personal_path("gtkg.stderr", pathname, sizeof pathname);
}

/**
 * Patch directory by replacing the leading "home" with the "personal"
 * directory if the supplied pathname does not exist.  If it exists, we have
 * to assume the original path was used, or created explicitely by the user
 * to be used, and we're not going to supersede it.
 *
 * @return the argument if nothing needs to be patched, a patched string
 * otherwise which needs to be freed via hfree().
 */
char *
mingw_patch_personal_path(const char *pathname)
{
	const char *home = mingw_get_home_path();
	const char *p;

	p = is_strprefix(pathname, home);
	if (p != NULL && !is_directory(pathname)) {
		char *patched;
		if (is_strsuffix(pathname, -1, "gtk-gnutella-downloads/complete")) {
			/* 
			 * Put the gtk-gnutella-downloads/complete into the downloads folder
			 * as this is where the user would expect completed downloads to be
			 * be placed
			 * 	-- JA 29/7/2011
			 */
			patched = h_strdup(
				g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD));
		} else {
			/*
			 * Put everything else under "My Documents/gtk-gnutella", were
			 * we should already find stdout and stderr files created when
			 * running from the GUI.
			 */

			patched = h_strconcat(mingw_get_personal_path(),
				G_DIR_SEPARATOR_S, product_get_name(), p, (void *) 0);
		}
		s_debug("patched \"%s\" into \"%s\"", pathname, patched);
		return patched;
	} else {
		return deconstify_char(pathname);	/* No need to patch anything */
	}
}

uint64
mingw_getphysmemsize(void)
{
	MEMORYSTATUSEX memStatus;

	memStatus.dwLength = sizeof memStatus;

	if (!GlobalMemoryStatusEx(&memStatus)) {
		errno = mingw_last_error();
		return -1;
	}
	return memStatus.ullTotalPhys;
}

int
mingw_getdtablesize(void)
{
	return _getmaxstdio();
}

int
mingw_mkdir(const char *pathname, mode_t mode)
{
	int res;
	pncs_t pncs;

	(void) mode; 	/* FIXME: handle mode */

	if (pncs_convert(&pncs, pathname))
		return -1;

	res = _wmkdir(pncs.utf16);
	if (-1 == res)
		errno = mingw_last_error();

	return res;
}

int
mingw_access(const char *pathname, int mode)
{
	int res;
	pncs_t pncs;

	if (pncs_convert(&pncs, pathname))
		return -1;

	res = _waccess(pncs.utf16, mode);
	if (-1 == res)
		errno = mingw_last_error();

	return res;
}

int
mingw_chdir(const char *pathname)
{
	int res;
	pncs_t pncs;

	if (pncs_convert(&pncs, pathname))
		return -1;

	res = _wchdir(pncs.utf16);
	if (-1 == res)
		errno = mingw_last_error();

	return res;
}

int
mingw_remove(const char *pathname)
{
	int res;
	pncs_t pncs;

	if (pncs_convert(&pncs, pathname))
		return -1;

	res = _wremove(pncs.utf16);
	if (-1 == res)
		errno = mingw_last_error();

	return res;
}

int
mingw_pipe(int fd[2])
{
	/* Buffer size of 8192 is arbitrary */
	return _pipe(fd, 8192, _O_BINARY);
}

int
mingw_stat(const char *pathname, filestat_t *buf)
{
	pncs_t pncs;
	int res;
   
	if (pncs_convert(&pncs, pathname))
		return -1;

	res = _wstati64(pncs.utf16, buf);
	if (-1 == res)
		errno = mingw_last_error();

	return res;
}

int
mingw_fstat(int fd, filestat_t *buf)
{
	int res;
   
	res = _fstati64(fd, buf);
	if (-1 == res)
		errno = mingw_last_error();

	return res;
}

int
mingw_unlink(const char *pathname)
{
	pncs_t pncs;
	int res;
   
	if (pncs_convert(&pncs, pathname))
		return -1;

	res = _wunlink(pncs.utf16);
	if (-1 == res)
		errno = mingw_last_error();

	return res;
}

int
mingw_dup2(int oldfd, int newfd)
{
	int res;
  
	if (oldfd == newfd) {
		/* Windows does not like dup2(fd, fd) */
		if (is_open_fd(oldfd))
			res = newfd;
		else
			res = -1;
	} else {
		res = dup2(oldfd, newfd);
		if (-1 == res)
			errno = mingw_last_error();
		else
			res = newfd;	/* Windows's dup2() returns 0 on success */
	}
	return res;
}

int
mingw_open(const char *pathname, int flags, ...)
{
	int res;
	mode_t mode = 0;
	pncs_t pncs;

	flags |= O_BINARY;
	if (flags & O_CREAT) {
        va_list  args;

        va_start(args, flags);
        mode = (mode_t) va_arg(args, int);
        va_end(args);
    }

	if (pncs_convert(&pncs, pathname))
		return -1;

	res = _wopen(pncs.utf16, flags, mode);
	if (-1 == res)
		errno = mingw_last_error();

	return res;
}

void *
mingw_opendir(const char *pathname)
{
	_WDIR *res;
	pncs_t pncs;

	if (pncs_convert(&pncs, pathname))
		return NULL;

	res = _wopendir(pncs.utf16);
	if (NULL == res)
		errno = mingw_last_error();

	return res;
}

void *
mingw_readdir(void *dir)
{
	struct _wdirent *res;

	res = _wreaddir(dir);
	if (NULL == res) {
		errno = mingw_last_error();
		return NULL;
	}
	return res;
}

int
mingw_closedir(void *dir)
{
	int res = _wclosedir(dir);
	if (-1 == res)
		errno = mingw_last_error();
	return 0;
}

/**
 * @note The returned UTF-8 string becomes invalid after the next
 *		 call to dir_entry_filename().
 *		 In order to avoid a memory leak, you may pass NULL as
 *		 parameter to free the memory.
 */
const char *
dir_entry_filename(const void *dirent)
{
	const struct _wdirent *wdirent = dirent;
	static char *filename;

	HFREE_NULL(filename);
	if (NULL != wdirent) {
		filename = utf16_to_utf8_string(wdirent->d_name);
	}
	return filename;
}

fileoffset_t
mingw_lseek(int fd, fileoffset_t offset, int whence)
{
	fileoffset_t res = _lseeki64(fd, offset, whence);
	if ((fileoffset_t) -1 == res)
		errno = mingw_last_error();
	return res;
}

ssize_t
mingw_read(int fd, void *buf, size_t count)
{
	ssize_t res;

	res = read(fd, buf, MIN(count, UINT_MAX));
	g_assert(-1 == res || (res >= 0 && UNSIGNED(res) <= count));
	
	if (-1 == res)
		errno = mingw_last_error();
	return res;
}

ssize_t
mingw_readv(int fd, iovec_t *iov, int iov_cnt)
{
    /*
     * Might want to use WriteFileGather here, however this probably has an
     * impact on the rest of the source code as well as this will be
     * unbuffered and async.
     */
	int i;
    ssize_t total_read = 0, r = -1;
	
	for (i = 0; i < iov_cnt; i++) {
		r = mingw_read(fd, iovec_base(&iov[i]), iovec_len(&iov[i]));

		if (-1 == r)
			break;

		g_assert(r >= (ssize_t)0);
		g_assert(r <= (ssize_t)iovec_len(&iov[i]));

		total_read += r;

		if (UNSIGNED(r) != iovec_len(&iov[i]))
			break;
	}

    return total_read > 0 ? total_read : r;
}

ssize_t
mingw_write(int fd, const void *buf, size_t count)
{
	ssize_t res = write(fd, buf, MIN(count, UINT_MAX));
	if (-1 == res)
		errno = mingw_last_error();
	return res;
}

ssize_t
mingw_writev(int fd, const iovec_t *iov, int iov_cnt)
{
    /*
     * Might want to use WriteFileGather here, however this probably has an
     * impact on the rest of the source code as well as this will be
     * unbuffered and async.
     */

	int i;
	ssize_t total_written = 0, w = -1;
	char gather[1024];
	size_t nw;

	/*
	 * Because logging routines expect the writev() call to be atomic,
	 * and since logging message are usually small, we gather the
	 * string in memory first if it fits our small buffer.
	 */

	nw = iov_calculate_size(iov, iov_cnt);
	if (nw <= sizeof gather) {
		char *p = gather;

		for (i = 0; i < iov_cnt; i++) {
			size_t n = iovec_len(&iov[i]);

			p = mempcpy(p, iovec_base(&iov[i]), n);
		}
		g_assert(ptr_diff(p, gather) <= sizeof gather);

		w = mingw_write(fd, gather, nw);
	} else {
		for (i = 0; i < iov_cnt; i++) {
			w = mingw_write(fd, iovec_base(&iov[i]), iovec_len(&iov[i]));

			if (-1 == w)
				break;

			total_written += w;

			if (UNSIGNED(w) != iovec_len(&iov[i]))
				break;
		}
	}

	return total_written > 0 ? total_written : w;
}

int
mingw_truncate(const char *pathname, fileoffset_t len)
{
	int fd;
	fileoffset_t offset;

	fd = mingw_open(pathname, O_RDWR);
	if (-1 == fd)
		return -1;

	offset = mingw_lseek(fd, len, SEEK_SET);
	if ((fileoffset_t)-1 == offset || offset != len) {
		int saved_errno = errno;
		fd_close(&fd);
		errno = saved_errno;
		return -1;
	}
	if (!SetEndOfFile((HANDLE) _get_osfhandle(fd))) {
		int saved_errno = mingw_last_error();
		fd_close(&fd);
		errno = saved_errno;
		return -1;
	}
	fd_close(&fd);
	return 0;
}

/***
 *** Socket wrappers
 ***/
 
int 
mingw_select(int nfds, fd_set *readfds, fd_set *writefds,
	fd_set *exceptfds, struct timeval *timeout)
{
	int res = select(nfds, readfds, writefds, exceptfds, timeout);
	
	if (res < 0)
		errno = mingw_wsa_last_error();
		
	return res;
}

int
mingw_getaddrinfo(const char *node, const char *service,
	const struct addrinfo *hints, struct addrinfo **res)
{
	int result = getaddrinfo(node, service, hints, res);
	if (result != 0)
		errno = mingw_wsa_last_error();
	return result;
}

void
mingw_freeaddrinfo(struct addrinfo *res)
{
	freeaddrinfo(res);
}

socket_fd_t
mingw_socket(int domain, int type, int protocol)
{
	socket_fd_t res;

	/* Initialize the socket layer */
	if G_UNLIKELY(!mingw_inited)
		mingw_init();

	/*
	 * Use WSASocket() to avoid creating "overlapped" sockets (i.e. sockets
	 * that can support asynchronous I/O).  This normally allows sockets to be
	 * use in read() and write() calls, transparently, as if they were files
	 * but it does not seem to work in the local_shell() code.
	 *
	 * It could however save on some system resources (avoiding creating and
	 * maintaining data structures that we won't be using anyway).
	 *		--RAM, 2011-01-11
	 */

	res = WSASocket(domain, type, protocol, NULL, 0, 0);
	if (INVALID_SOCKET == res)
		errno = mingw_wsa_last_error();
	return res;
}

int
mingw_bind(socket_fd_t sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	int res;

	/* Initialize the socket layer */
	if G_UNLIKELY(!mingw_inited)
		mingw_init();

	res = bind(sockfd, addr, addrlen);
	if (-1 == res)
		errno = mingw_wsa_last_error();
	return res;
}

socket_fd_t
mingw_connect(socket_fd_t sockfd, const struct sockaddr *addr,
	  socklen_t addrlen)
{
	socket_fd_t res;

	/* Initialize the socket layer */
	if G_UNLIKELY(!mingw_inited)
		mingw_init();

	res = connect(sockfd, addr, addrlen);
	if (INVALID_SOCKET == res)
		errno = mingw_wsa_last_error();
	return res;
}

int
mingw_listen(socket_fd_t sockfd, int backlog)
{
	int res;

	/* Initialize the socket layer */
	if G_UNLIKELY(!mingw_inited)
		mingw_init();

	res = listen(sockfd, backlog);
	if (-1 == res)
		errno = mingw_wsa_last_error();
	return res;
}

socket_fd_t
mingw_accept(socket_fd_t sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	socket_fd_t res;

	/* Initialize the socket layer */
	if G_UNLIKELY(!mingw_inited)
		mingw_init();

	res = accept(sockfd, addr, addrlen);
	if (INVALID_SOCKET == res)
		errno = mingw_wsa_last_error();
	return res;
}

int
mingw_shutdown(socket_fd_t sockfd, int how)
{

	int res;

	/* Initialize the socket layer */
	if G_UNLIKELY(!mingw_inited)
		mingw_init();

	res = shutdown(sockfd, how);
	if (-1 == res)
		errno = mingw_wsa_last_error();
	return res;
}

int
mingw_getsockopt(socket_fd_t sockfd, int level, int optname,
	void *optval, socklen_t *optlen)
{
	int res;

	/* Initialize the socket layer */
	if G_UNLIKELY(!mingw_inited)
		mingw_init();

	res = getsockopt(sockfd, level, optname, optval, optlen);
	if (-1 == res)
		errno = mingw_wsa_last_error();
	return res;
}

int
mingw_setsockopt(socket_fd_t sockfd, int level, int optname,
	  const void *optval, socklen_t optlen)
{
	int res;
	
	/* Initialize the socket layer */
	if G_UNLIKELY(!mingw_inited)
		mingw_init();

	res = setsockopt(sockfd, level, optname, optval, optlen);
	if (-1 == res)
		errno = mingw_wsa_last_error();
	return res;
}


ssize_t
s_write(socket_fd_t fd, const void *buf, size_t count)
{
	ssize_t res;

 	count = MIN(count, UNSIGNED(INT_MAX));	
	res = send(fd, buf, count, 0);
	if (-1 == res)
		errno = mingw_wsa_last_error();
	return res;
}

ssize_t
s_read(socket_fd_t fd, void *buf, size_t count)
{
	ssize_t res;
   
 	count = MIN(count, UNSIGNED(INT_MAX));	
	res = recv(fd, buf, count, 0);
	if (-1 == res)
		errno = mingw_wsa_last_error();
	return res;
}

int
s_close(socket_fd_t fd)
{
	int res = closesocket(fd);
	if (-1 == res)
		errno = mingw_wsa_last_error();
	return res;
}

ssize_t
mingw_recv(socket_fd_t fd, void *buf, size_t len, int recv_flags)
{
	DWORD r, flags = recv_flags;
	iovec_t iov;
	int res;

	iovec_set_base(&iov, buf);
	iovec_set_len(&iov, len);

	res = WSARecv(fd, (LPWSABUF) &iov, 1, &r, &flags, NULL, NULL);

	if (res != 0) {
		errno = mingw_wsa_last_error();
		return (ssize_t) -1;
	}
	return (ssize_t) r;
}

ssize_t
mingw_s_readv(socket_fd_t fd, const iovec_t *iov, int iovcnt)
{
	DWORD r, flags = 0;
	int res = WSARecv(fd, (LPWSABUF) iov, iovcnt, &r, &flags, NULL, NULL);

	if (res != 0) {
		errno = mingw_wsa_last_error();
		return (ssize_t) -1;
	}
	return (ssize_t) r;
}

ssize_t
mingw_s_writev(socket_fd_t fd, const iovec_t *iov, int iovcnt)
{
	DWORD w;
	int res = WSASend(fd, (LPWSABUF) iov, iovcnt, &w, 0, NULL, NULL);

	if (res != 0) {
		errno = mingw_wsa_last_error();
		return (ssize_t) -1;
	}
	return (ssize_t) w;
};

#if HAS_WSARECVMSG
/* FIXME: WSARecvMsg is not included in MingW yet */
ssize_t
mingw_recvmsg(socket_fd_t s, struct msghdr *hdr, int flags)
{
	DWORD received;
	WSAMSG msg;
	int res;

	msg.name = hdr->msg_name;
	msg.namelen = hdr->msg_namelen;
	msg.lpBuffers = hdr->msg_iov;
	msg.dwBufferCount = hdr->msg_iovlen;
	msg.Control.len = hdr->msg_controllen;
	msg.Control.buf = hdr->msg_control;
	msg.dwFlags = hdr->msg_flags;

	res = WSARecvMsg(s, &msg, &received, NULL, NULL);
	if (res != 0) {
		errno = mingw_wsa_last_error();
		return -1;
	}
	return received;
}	
#endif	/* HAS_WSARECVMSG */

ssize_t
mingw_recvfrom(socket_fd_t s, void *data, size_t len, int flags,
	struct sockaddr *src_addr, socklen_t *addrlen)
{
	DWORD received, dflags = flags;
	WSABUF buf;
	INT ifromLen = *addrlen;
	int res;

 	len = MIN(len, UNSIGNED(INT_MAX));	
	buf.buf = data;
	buf.len = len;
	res = WSARecvFrom(s, &buf, 1, &received, &dflags,
			src_addr, &ifromLen, NULL, NULL);
	if (0 != res) {
		errno = mingw_wsa_last_error();
		return -1;
	}
	*addrlen = ifromLen;
	/* Not sure about behaviour on truncation */
	g_return_val_if_fail(received <= len, len);
	return received;
}

ssize_t
mingw_sendto(socket_fd_t sockfd, const void *buf, size_t len, int flags,
	  const struct sockaddr *dest_addr, socklen_t addrlen)
{
	ssize_t res;
	
 	len = MIN(len, UNSIGNED(INT_MAX));	
	res = sendto(sockfd, buf, len, flags, dest_addr, addrlen);
	if (-1 == res)
		errno = mingw_wsa_last_error();
	return res;
}

/***
 *** Memory allocation routines.
 ***/

static struct {
	void *reserved;			/* Reserved memory */
	size_t size;			/* Size for hinted allocation */
	size_t later;			/* Size of "later" memory we did not reserve */
	int hinted;
} mingw_vmm;

void *
mingw_valloc(void *hint, size_t size)
{
	void *p = NULL;

	if (NULL == hint && mingw_vmm.hinted >= 0) {
		size_t n;

		if G_UNLIKELY(NULL == mingw_vmm.reserved) {
			MEMORYSTATUSEX memStatus;
			SYSTEM_INFO system_info;
			void *mem_later;
			size_t mem_latersize;
			size_t mem_size;

			/* Determine maximum possible memory first */

			GetNativeSystemInfo(&system_info);

			mingw_vmm.size =
				system_info.lpMaximumApplicationAddress
				-
				system_info.lpMinimumApplicationAddress;

			memStatus.dwLength = sizeof memStatus;
			if (GlobalMemoryStatusEx(&memStatus)) {
				if (memStatus.ullTotalPhys < mingw_vmm.size)
					mingw_vmm.size = memStatus.ullTotalPhys;
			}

			/*
			 * Declare some space for future allocations without hinting.
			 * We initially reserve 1/4th of the virtual address space,
			 * with VMM_MINSIZE at least but we make sure we have more room 
			 * available for the VMM layer.
			 *
			 * We'll iterate, dividing the amount of memory we keep for later
			 * allocations by half at each loop until we can reserve more
			 * memory for GTKG than the amount we leave to the system or other
			 * non-hinted allocations.
			 */

			mem_size = mingw_vmm.size;		/* For the VMM layer */
			mem_latersize = mem_size / 2;	/* For non-hinted allocation */

			do {
				if (mingw_vmm.reserved != NULL) {
					VirtualFree(mingw_vmm.reserved, 0, MEM_RELEASE);
					mingw_vmm.reserved = NULL;
				}

				mem_latersize /= 2;
				mem_latersize = MAX(mem_latersize, VMM_MINSIZE);
				mingw_vmm.later = mem_latersize;
				mem_later = VirtualAlloc(NULL,
					mem_latersize, MEM_RESERVE, PAGE_NOACCESS);

				if (NULL == mem_later) {
					errno = mingw_last_error();
					s_error("could not reserve %s of memory: %m",
						compact_size(mem_latersize, FALSE));
				}

				/*
				 * Try to reserve the remaining virtual space, asking for as
				 * much as we can and reducing the requested size by the
				 * system's granularity until we get a success status.
				 */

				mingw_vmm.size = mem_size;

				while (
					NULL == mingw_vmm.reserved && mingw_vmm.size > VMM_MINSIZE
				) {
					mingw_vmm.reserved = p = VirtualAlloc(
						NULL, mingw_vmm.size, MEM_RESERVE, PAGE_NOACCESS);

					if (NULL == mingw_vmm.reserved)
						mingw_vmm.size -= system_info.dwAllocationGranularity;
				}

				VirtualFree(mem_later, 0, MEM_RELEASE);
			} while (
				mingw_vmm.size > VMM_MINSIZE && mingw_vmm.size < mem_latersize
			);

			if (NULL == mingw_vmm.reserved) {
				s_error("could not reserve additional %s of memory "
					"on top of the %s put aside",
					compact_size(mingw_vmm.size, FALSE),
					compact_size2(mem_latersize, FALSE));
			}
		}

		if (vmm_is_debugging(0)) {
			s_debug("no hint given for %s allocation #%d",
				compact_size(size, FALSE), mingw_vmm.hinted);
		}

		n = mingw_getpagesize();
		n = size_saturate_mult(n, mingw_vmm.hinted++);
		if (n + size >= mingw_vmm.size) {
			s_carp("%s(): out of reserved memory for %zu bytes",
				G_STRFUNC, size);
			goto failed;
		}
		p = ptr_add_offset(mingw_vmm.reserved, n);
	} else if (NULL == hint && mingw_vmm.hinted < 0) {
		/*
		 * Non-hinted request after hinted requests have been used.
		 * Allow usage of non-reserved space.
		 */

		p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

		if (p == NULL) {
			errno = mingw_last_error();
			s_carp("%s(): failed to allocate %zu bytes: %m", G_STRFUNC, size);
			goto failed;
		}
		return p;
	} else {
		mingw_vmm.hinted = -1;	/* Can now handle non-hinted allocs */
		p = hint;
	}

	p = VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE);

	if (p == NULL) {
		errno = mingw_last_error();
		s_carp("%s(): failed to commit %zu bytes at %p: %m",
			G_STRFUNC, size, hint);
		goto failed;
	}

	return p;

failed:
	errno = ENOMEM;		/* Expected errno value from VMM layer */
	return MAP_FAILED;
}

int
mingw_vfree(void *addr, size_t size)
{
	(void) addr;
	(void) size;

	/*
	 * VMM hint should always be respected. So this function should not
	 * be reached from VMM, ever.
	 */

	g_assert_not_reached();
}

int
mingw_vfree_fragment(void *addr, size_t size)
{
	void *end = ptr_add_offset(mingw_vmm.reserved, mingw_vmm.size);

	if (ptr_cmp(mingw_vmm.reserved, addr) <= 0 && ptr_cmp(end, addr) > 0) {
		/* Allocated in reserved space */
		if (!VirtualFree(addr, size, MEM_DECOMMIT)) {
			errno = mingw_last_error();
			return -1;
		}
	} else if (!VirtualFree(addr, 0, MEM_RELEASE)) {
		/* Allocated in non-reserved space */
		errno = mingw_last_error();
		return -1;
	}

	return 0;
}

int
mingw_mprotect(void *addr, size_t len, int prot)
{
	DWORD oldProtect = 0;
	DWORD newProtect;
	BOOL res;

	switch (prot) {
	case PROT_NONE:
		newProtect = PAGE_NOACCESS;
		break;
	case PROT_READ:
		newProtect = PAGE_READONLY;
		break;
	case PROT_READ | PROT_WRITE:
		newProtect = PAGE_READWRITE;
		break;
	default:
		g_carp("mingw_mprotect(): unsupported protection flags 0x%x", prot);
		res = EINVAL;
		return -1;
	}

	res = VirtualProtect((LPVOID) addr, len, newProtect, &oldProtect);
	if (!res) {
		errno = mingw_last_error();
		if (vmm_is_debugging(0)) {
			s_debug("VMM mprotect(%p, %zu) failed: errno=%m", addr, len);
		}
		return -1;
	}

	return 0;	/* OK */
}

/***
 *** Random numbers.
 ***/

/**
 * Fill supplied buffer with random bytes.
 * @return amount of generated random bytes.
 */
int
mingw_random_bytes(void *buf, size_t len)
{
	HCRYPTPROV crypth = 0;

	g_assert(len <= MAX_INT_VAL(int));

	if (
		!CryptAcquireContext(&crypth, NULL, NULL, PROV_RSA_FULL,
			CRYPT_VERIFYCONTEXT | CRYPT_SILENT)
	) {
		errno = mingw_last_error();
		return 0;
	}

	memset(buf, 0, len);
	if (!CryptGenRandom(crypth, len, buf)) {
		errno = mingw_last_error();
		len = 0;
	}
	CryptReleaseContext(crypth, 0);

	return (int) len;
}

/***
 *** Miscellaneous.
 ***/

static const char *
mingw_posix_strerror(int errnum)
{
	switch (errnum) {
	case EPERM:		return "Operation not permitted";
	case ENOFILE:	return "No such file or directory";
	/* ENOENT is a duplicate for ENOFILE */
	case ESRCH:		return "No such process";
	case EINTR:		return "Interrupted function call";
	case EIO:		return "Input/output error";
	case ENXIO:		return "No such device or address";
	case E2BIG:		return "Arg list too long";
	case ENOEXEC:	return "Exec format error";
	case EBADF:		return "Bad file descriptor";
	case ECHILD:	return "No child process";
	case EAGAIN:	return "Resource temporarily unavailable";
	case ENOMEM:	return "Not enough memory space";
	case EACCES:	return "Access denied";
	case EFAULT:	return "Bad address";
	case EBUSY:		return "Device busy";
	case EEXIST:	return "File already exists";
	case EXDEV:		return "Improper link";
	case ENODEV:	return "No such device";
	case ENOTDIR:	return "Not a directory";
	case EISDIR:	return "Is a directory";
	case EINVAL:	return "Invalid argument";
	case ENFILE:	return "Too many open files in system";
	case EMFILE:	return "Too many open files in the process";
	case ENOTTY:	return "Not a tty";
	case EFBIG:		return "File too large";
	case ENOSPC:	return "No space left on device";
	case ESPIPE:	return "Invalid seek on pipe";
	case EROFS:		return "Read-only file system";
	case EMLINK:	return "Too many links";
	case EPIPE:		return "Broken pipe";
	case EDOM:		return "Domain error";		/* Math */
	case ERANGE:	return "Result out of range";
	case EDEADLK:	return "Resource deadlock avoided";
	case ENAMETOOLONG:	return "Filename too long";
	case ENOLCK:	return "No locks available";
	case ENOSYS:	return "Function not implemented";
	case ENOTEMPTY:	return "Directory not empty";
	case EILSEQ:	return "Illegal byte sequence";
	case EOVERFLOW:	return "Value too large to be stored in data type";
	default:		return NULL;
	}

	g_assert_not_reached();
}

const char *
mingw_strerror(int errnum)
{
	const char *msg;
	static char strerrbuf[1024];

	/*
	 * We have one global "errno" but conflicting ranges for errors: the
	 * POSIX ones defined in MinGW overlap with some of the Windows error
	 * codes.
	 *
	 * Because our code is POSIX, we strive to remap these conflicting codes
	 * to POSIX values and naturally provide our own strerror() for the
	 * POSIX errors.
	 */

	msg = mingw_posix_strerror(errnum);
	if (msg != NULL)
		return msg;

	FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, errnum,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) strerrbuf,
        sizeof strerrbuf, NULL );

	strchomp(strerrbuf, 0);		/* Remove final "\r\n" */
	return strerrbuf;
}

int
mingw_rename(const char *oldpathname, const char *newpathname)
{
	pncs_t old, new;
	int res;

	if (pncs_convert(&old, oldpathname))
		return -1;

	if (pncs_convert(&new, newpathname))
		return -1;

	/*
	 * FIXME: Try to rename a file with SetFileInformationByHandle
	 * and FILE_INFO_BY_HANDLE_CLASS
	 */

	if (MoveFileExW(old.utf16, new.utf16, MOVEFILE_REPLACE_EXISTING)) {
		res = 0;
	} else {
		errno = mingw_last_error();
		res = -1;
	}

	return res;
}

FILE *
mingw_fopen(const char *pathname, const char *mode)
{
	pncs_t wpathname;
	char bin_mode[14];
	wchar_t wmode[32];
	FILE *res;

	if (NULL == strchr(mode, 'b')) {
		int l = clamp_strcpy(bin_mode, sizeof bin_mode - 2, mode);
		bin_mode[l++] = 'b';
		bin_mode[l] = '\0';
		mode = bin_mode;
	}
	
	if (pncs_convert(&wpathname, pathname))
		return NULL;

	if (
		!is_ascii_string(mode) ||
		utf8_to_utf16(mode, wmode, G_N_ELEMENTS(wmode)) >=
			G_N_ELEMENTS(wmode)
	) {
		errno = EINVAL;
		return NULL;
	}

	res = _wfopen(wpathname.utf16, wmode);
	if (NULL == res)
		errno = mingw_last_error();

	return res;
}

FILE *
mingw_freopen(const char *pathname, const char *mode, FILE *file)
{
	pncs_t wpathname;
	char bin_mode[14];
	wchar_t wmode[32];
	FILE *res;

	if (pncs_convert(&wpathname, pathname))
		return NULL;

	if (NULL == strchr(mode, 'b')) {
		int l = clamp_strcpy(bin_mode, sizeof bin_mode - 2, mode);
		bin_mode[l++] = 'b';
		bin_mode[l] = '\0';
		mode = bin_mode;
	}
	
	if (
		!is_ascii_string(mode) ||
		utf8_to_utf16(mode, wmode, G_N_ELEMENTS(wmode)) >=
			G_N_ELEMENTS(wmode)
	) {
		errno = EINVAL;
		return NULL;
	}

	res = _wfreopen(wpathname.utf16, wmode, file);
	if (NULL == res)
		errno = mingw_last_error();
	return res;
}

int
mingw_statvfs(const char *pathname, struct mingw_statvfs *buf)
{
	BOOL ret;
	DWORD SectorsPerCluster;
	DWORD BytesPerSector;
	DWORD NumberOfFreeClusters;
	DWORD TotalNumberOfClusters;
	pncs_t pncs;

	if (pncs_convert(&pncs, pathname))
		return -1;

	ret = GetDiskFreeSpaceW(pncs.utf16,
		&SectorsPerCluster, &BytesPerSector,
		&NumberOfFreeClusters,
		&TotalNumberOfClusters);

	if (!ret) {
		errno = mingw_last_error();
		return -1;
	}

	buf->f_csize = SectorsPerCluster * BytesPerSector;
	buf->f_clusters = TotalNumberOfClusters;
	buf->f_cavail = NumberOfFreeClusters;

	return 0;
}

#ifdef EMULATE_SCHED_YIELD
/**
 * Cause the calling thread to relinquish the CPU.
 */
int
mingw_sched_yield(void)
{
	Sleep(0);
	return 0;
}
#endif	/* EMULATE_SCHED_YIELD */

#ifdef EMULATE_GETRUSAGE
/**
 * Convert a FILETIME into a timeval.
 *
 * @param ft		the FILETIME structure to convert
 * @param tv		the struct timeval to fill in
 * @param offset	offset to substract to the FILETIME value
 */
static void
mingw_filetime_to_timeval(const FILETIME *ft, struct timeval *tv, uint64 offset)
{
	uint64 v;

	/*
	 * From MSDN documentation:
	 *
	 * A FILETIME Contains a 64-bit value representing the number of
	 * 100-nanosecond intervals since January 1, 1601 (UTC).
	 *
	 * All times are expressed using FILETIME data structures.
	 * Such a structure contains two 32-bit values that combine to form
	 * a 64-bit count of 100-nanosecond time units.
	 *
	 * It is not recommended that you add and subtract values from the
	 * FILETIME structure to obtain relative times. Instead, you should copy
	 * the low- and high-order parts of the file time to a ULARGE_INTEGER
	 * structure, perform 64-bit arithmetic on the QuadPart member, and copy
	 * the LowPart and HighPart members into the FILETIME structure.
	 */

	v = (ft->dwLowDateTime | ((ft->dwHighDateTime + (uint64) 0) << 32)) / 10;
	v -= offset;
	tv->tv_usec = v % 1000000UL;
	v /= 1000000UL;
	/* If time_t is a 32-bit integer, there could be an overflow */
	tv->tv_sec = MIN(v, UNSIGNED(MAX_INT_VAL(time_t)));
}

int
mingw_getrusage(int who, struct rusage *usage)
{
	FILETIME creation_time, exit_time, kernel_time, user_time;

	if (who != RUSAGE_SELF) {
		errno = EINVAL;
		return -1;
	}
	if (NULL == usage) {
		errno = EACCES;
		return -1;
	}

	if (
		0 == GetProcessTimes(GetCurrentProcess(),
			&creation_time, &exit_time, &kernel_time, &user_time)
	) {
		errno = mingw_last_error();
		return -1;
	}

	mingw_filetime_to_timeval(&user_time, &usage->ru_utime, 0);
	mingw_filetime_to_timeval(&kernel_time, &usage->ru_stime, 0);

	return 0;
}
#endif	/* EMULATE_GETRUSAGE */

const char *
mingw_getlogin(void)
{
	static char buf[128];
	static char *result;
	static bool inited;
	DWORD size;

	if (G_LIKELY(inited))
		return result;

	size = sizeof buf;
	result = 0 == GetUserName(buf, &size) ? NULL : buf;

	inited = TRUE;
	return result;
}

int
mingw_getpagesize(void)
{
	static int result;
	SYSTEM_INFO system_info;

	if (G_LIKELY(result != 0))
		return result;

	GetSystemInfo(&system_info);
	return result = system_info.dwPageSize;
}

/**
 * Compute the system processor architecture, once.
 */
static int
mingw_proc_arch(void)
{
	static SYSTEM_INFO system_info;
	static bool done;

	if (done)
		return system_info.wProcessorArchitecture;

	done = TRUE;
	GetNativeSystemInfo(&system_info);
	return system_info.wProcessorArchitecture;
}


#ifdef EMULATE_UNAME
int
mingw_uname(struct utsname *buf)
{
	OSVERSIONINFO osvi;
	DWORD len;
	const char *cpu;

	ZERO(buf);

	g_strlcpy(buf->sysname, "Windows", sizeof buf->sysname);

	switch (mingw_proc_arch()) {
	case PROCESSOR_ARCHITECTURE_AMD64:	cpu = "x64"; break;
	case PROCESSOR_ARCHITECTURE_IA64:	cpu = "ia64"; break;
	case PROCESSOR_ARCHITECTURE_INTEL:	cpu = "x86"; break;
	default:							cpu = "unknown"; break;
	}
	g_strlcpy(buf->machine, cpu, sizeof buf->machine);

	osvi.dwOSVersionInfoSize = sizeof osvi;
	if (GetVersionEx(&osvi)) {
		gm_snprintf(buf->release, sizeof buf->release, "%u.%u",
			(unsigned) osvi.dwMajorVersion, (unsigned) osvi.dwMinorVersion);
		gm_snprintf(buf->version, sizeof buf->version, "%u %s",
			(unsigned) osvi.dwBuildNumber, osvi.szCSDVersion);
	}

	len = sizeof buf->nodename;
	GetComputerName(buf->nodename, &len);

	return 0;
}
#endif	/* EMULATE_UNAME */

#ifdef EMULATE_NANOSLEEP
int
mingw_nanosleep(const struct timespec *req, struct timespec *rem)
{
	static HANDLE t = NULL;
	LARGE_INTEGER dueTime;
	uint64 value;

	/*
	 * There's no residual time, there cannot be early terminations.
	 */

	if (NULL != rem) {
		rem->tv_sec = 0;
		rem->tv_nsec = 0;
	}

	if (G_UNLIKELY(NULL == t)) {
		t = CreateWaitableTimer(NULL, TRUE, NULL);

		if (NULL == t)
			g_carp("unable to create waitable timer, ignoring nanosleep()");

		errno = ENOMEM;		/* System problem anyway */
		return -1;
	}

	if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec > 999999999L) {
		errno = EINVAL;
		return -1;
	}

	if (0 == req->tv_sec && 0 == req->tv_nsec)
		return 0;

	/*
	 * For Windows, the time specification unit is 100 nsec.
	 * We therefore round up the amount of nanoseconds to the nearest value.
	 * Negative values indicate relative time.
	 */

	value = uint64_saturate_add(
				uint64_saturate_mult(req->tv_sec, 10000000UL),
				(req->tv_nsec + 99) / 100);
	dueTime.QuadPart = -MIN(value, MAX_INT_VAL(gint64));

	if (0 == SetWaitableTimer(t, &dueTime, 0, NULL, NULL, FALSE)) {
		errno = mingw_last_error();
		s_carp("could not set timer, unable to nanosleep(): %m");
		return -1;
	}

	if (WaitForSingleObject(t, INFINITE) != WAIT_OBJECT_0) {
		s_warning("timer returned an unexpected value, nanosleep() failed");
		errno = EINTR;
		return -1;
	}

	return 0;
}
#endif

bool
mingw_process_is_alive(pid_t pid)
{
	char our_process_name[1024];
	char process_name[1024];
	HANDLE p;
	BOOL res = FALSE;

	pid_t our_pid = GetCurrentProcessId();

	/* PID might be reused */
	if (our_pid == pid)
		return FALSE;

	p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);

	if (NULL != p) {
		GetModuleBaseName(p, NULL, process_name, sizeof process_name);
		GetModuleBaseName(GetCurrentProcess(),
			NULL, our_process_name, sizeof our_process_name);

		res = g_strcmp0(process_name, our_process_name) == 0;
		CloseHandle(p);
    }

	return res;
}

long
mingw_cpu_count(void)
{
	static long result;
	SYSTEM_INFO system_info;

	if G_UNLIKELY(0 == result) {
		GetSystemInfo(&system_info);
		result = system_info.dwNumberOfProcessors;
		g_assert(result > 0);
	}
	return result;
}

uint64
mingw_cpufreq(enum mingw_cpufreq freq)
{
	unsigned long cpus = mingw_cpu_count();
	PROCESSOR_POWER_INFORMATION *p, powarray[16];
	size_t len;
	uint64 result = 0;

	len = size_saturate_mult(cpus, sizeof *p);
	if (cpus <= G_N_ELEMENTS(powarray)) {
		p = powarray;
	} else {
		p = walloc(len);
	}

	if (0 == CallNtPowerInformation(ProcessorInformation, NULL, 0, p, len)) {
		/* FIXME: In case of mulitple CPUs (or cores?) they can likely
		 *		  have different values especially for the current freq
		 */
		switch (freq) {
		case MINGW_CPUFREQ_CURRENT:
			/* Convert to Hz */
			result = uint64_saturate_mult(p[0].CurrentMhz, 1000000UL);
			break;
		case MINGW_CPUFREQ_MAX:
			/* Convert to Hz */
			result = uint64_saturate_mult(p[0].MaxMhz, 1000000UL);
			break;
		}
	}

	if (p != powarray)
		wfree(p, len);

	return result;
}

/***
 *** ADNS
 ***
 *** Functions ending with _thread are executed in the context of the ADNS
 *** thread, others are executed in the context of the main thread.
 ***
 *** All logging within the thread must use the t_xxx() logging routines
 *** in order to be thread-safe.
 ***/

static GAsyncQueue *mingw_gtkg_main_async_queue;
static GAsyncQueue *mingw_gtkg_adns_async_queue;
static volatile bool mingw_adns_thread_run;

struct async_data {
	void *user_data;
	
	void *thread_return_data;
	void *thread_arg_data;
	
	void (*thread_func)(struct async_data *);
	void (*callback_func)(struct async_data *);
};

struct arg_data {
	const struct sockaddr *sa;
	union {
		struct sockaddr_in sa_inet4;
		struct sockaddr_in6 sa_inet6;
	} u;
	char hostname[NI_MAXHOST];
	char servinfo[NI_MAXSERV];
};

/* ADNS getaddrinfo */

/**
 * ADNS getaddrinfo on ADNS thread.
 */
static void
mingw_adns_getaddrinfo_thread(struct async_data *ad)
{
	struct addrinfo *results;
	const char *hostname = ad->thread_arg_data;
	
	if (common_dbg > 1) {
		t_debug("ADNS resolving '%s'", hostname);
	}	
	getaddrinfo(hostname, NULL, NULL, &results);

	if (common_dbg > 1) {
		t_debug("ADNS got result for '%s' @%p", hostname, results);
	}
	ad->thread_return_data = results;	
}

/**
 * ADNS getaddrinfo callback function.
 */
static void
mingw_adns_getaddrinfo_cb(struct async_data *ad)
{
	struct adns_request *req;
	struct addrinfo *response;
	host_addr_t addrs[10];
	unsigned i;

	if (common_dbg > 2)
		s_debug("mingw_adns_getaddrinfo_cb");
		
	g_assert(ad);
	g_assert(ad->user_data);
	g_assert(ad->thread_arg_data);
	
	req = ad->user_data;
	response = ad->thread_return_data;
	
	for (i = 0; i < G_N_ELEMENTS(addrs); i++) {
		if (NULL == response)
			break;

		addrs[i] = addrinfo_to_addr(response);						
		if (common_dbg) {	
			s_debug("ADNS got %s for hostname %s",
				host_addr_to_string(addrs[i]),
				(const char *) ad->thread_arg_data);
		}
		response = response->ai_next;
	}
	
	{
		adns_callback_t func = (adns_callback_t) req->common.user_callback;
		g_assert(NULL != func);
		if (common_dbg) {
			s_debug("ADNS performing user-callback to %p with %u results", 
				req->common.user_data, i);
		}
		func(addrs, i, req->common.user_data);		
	}
	
	if (NULL != ad->thread_return_data) {
		freeaddrinfo(ad->thread_return_data);
		ad->thread_return_data = NULL;
	}
	HFREE_NULL(ad->thread_arg_data);
	WFREE(ad);
	HFREE_NULL(req);
}

/**
 * ADNS getaddrinfo. Retrieves DNS info by hostname. Returns multiple 
 * @see host_addr_t in the callbackfunction.
 *
 * Performs a hostname lookup on the ADNS thread. Thread function is set to 
 * @see mingw_adns_getaddrinfo_thread, which will call the 
 * @see mingw_adns_getaddrinfo_cb function on completion. The 
 * mingw_adns_getaddrinfo_cb is responsible for performing the user callback.
 * 
 * @param req The adns request, where:
 *		- req->query.by_addr.hostname the hostname to lookup.
 *		- req->common.user_callback, a @see adns_callback_t callback function 
 *		  pointer. Raised on completion.
 */
static void 
mingw_adns_getaddrinfo(const struct adns_request *req)
{
	struct async_data *ad;
	
	if (common_dbg > 2) {
		s_debug("%s", G_STRFUNC);
	}	
	g_assert(req);
	g_assert(req->common.user_callback);
	
	WALLOC0(ad);
	ad->thread_func = mingw_adns_getaddrinfo_thread;
	ad->callback_func = mingw_adns_getaddrinfo_cb;	
	ad->user_data = hcopy(req, sizeof *req);
	ad->thread_arg_data = h_strdup(req->query.by_addr.hostname);	
	
	g_async_queue_push(mingw_gtkg_adns_async_queue, ad);
}

/* ADNS Get name info */

/**
 * ADNS getnameinfo on ADNS thread.
 */
static void
mingw_adns_getnameinfo_thread(struct async_data *ad)
{
	struct arg_data *arg_data = ad->thread_arg_data;
	
	getnameinfo(arg_data->sa, sizeof arg_data->u,
		arg_data->hostname, sizeof arg_data->hostname,
		arg_data->servinfo, sizeof arg_data->servinfo, 
		NI_NUMERICSERV);

	t_debug("ADNS resolved to %s", arg_data->hostname);
}

/**
 * ADNS getnameinfo callback function.
 */
static void
mingw_adns_getnameinfo_cb(struct async_data *ad)
{
	struct adns_request *req = ad->user_data;
	struct arg_data *arg_data = ad->thread_arg_data;

	if (common_dbg) {	
		s_debug("ADNS resolved to %s", arg_data->hostname);
	}
	
	{
		adns_reverse_callback_t func =
			(adns_reverse_callback_t) req->common.user_callback;
		s_debug("ADNS getnameinfo performing user-callback to %p with %s", 
			req->common.user_data, arg_data->hostname);
		func(arg_data->hostname, req->common.user_data);
	}
	
	HFREE_NULL(req);
	WFREE(arg_data);
}

/**
 * ADNS getnameinfo. Retrieves DNS info by ip address. Returns the hostname in 
 * the callbackfunction.
 *
 * Performs a reverse hostname lookup on the ADNS thread. Thread function is 
 * set to @see mingw_adns_getnameinfo_thread, which will call the 
 * @see mingw_adns_getnameinfo_cb function on completion. The 
 * mingw_adns_getnameinfo_cb is responsible for performing the user callback.
 * 
 * @param req The adns request, where:
 *		- req->query.reverse.addr.net == @see NET_TYPE_IPV6 or
 *		  @see NET_TYPE_IPV4
 *		- req->query.addr.addr.ipv6 the ipv6 address if NET_TYPE_IPV6
 *		- req->query.addr.addr.ipv4 the ipv4 address if NET_TYPE_IPV4
 *		- req->common.user_callback, a @see adns_callback_t callback function 
 *		  pointer. Raised on completion.
 */
static void
mingw_adns_getnameinfo(const struct adns_request *req)
{
	const struct adns_reverse_query *query = &req->query.reverse;
	struct async_data *ad;
	struct arg_data *arg_data;

	WALLOC0(ad);
	WALLOC(arg_data);
	ad->thread_func = mingw_adns_getnameinfo_thread;
	ad->callback_func = mingw_adns_getnameinfo_cb;	
	ad->user_data = hcopy(req, sizeof *req);
	ad->thread_arg_data = arg_data;
	
	switch (query->addr.net) {
		struct sockaddr_in *inet4;
		struct sockaddr_in6 *inet6;
	case NET_TYPE_IPV6:
		inet6 = &arg_data->u.sa_inet6;
		inet6->sin6_family = AF_INET6;
		memcpy(inet6->sin6_addr.s6_addr, query->addr.addr.ipv6, 16);
		arg_data->sa = (const struct sockaddr *) inet6;
		break;
	case NET_TYPE_IPV4:
		inet4 = &arg_data->u.sa_inet4;
		inet4->sin_family = AF_INET;	
		inet4->sin_addr.s_addr = htonl(query->addr.addr.ipv4);
		arg_data->sa = (const struct sockaddr *) inet4;
		break;
	case NET_TYPE_LOCAL:
	case NET_TYPE_NONE:
		g_assert_not_reached();
		break;
	}	

	g_async_queue_push(mingw_gtkg_adns_async_queue, ad);
}

/* ADNS Main thread */

static void *
mingw_adns_thread(void *unused_data)
{
	GAsyncQueue *read_queue, *result_queue;
	
	/* On ADNS thread */
	(void) unused_data;

	read_queue = g_async_queue_ref(mingw_gtkg_adns_async_queue);
	result_queue = g_async_queue_ref(mingw_gtkg_main_async_queue);
	mingw_adns_thread_run = TRUE;
	
	while (mingw_adns_thread_run) {
		struct async_data *ad = g_async_queue_pop(read_queue);	

		if (NULL == ad)
			break;

		ad->thread_func(ad);
		g_async_queue_push(result_queue, ad);			
	}

	if (common_dbg)
		t_message("adns thread exit");

	/*
	 * FIXME: The calls below cause a:
	 *
	 *    assertion `g_atomic_int_get (&queue->ref_count) > 0' failed
	 *
	 * I'm wondering whether they are needed since the main thread does
	 * it and the queue could be disposed of by g_async_queue_pop() directly,
	 * given it can detect the queue became orphan....
	 */

#if 0
	g_async_queue_unref(mingw_gtkg_adns_async_queue);
	g_async_queue_unref(mingw_gtkg_main_async_queue);
#endif

	g_thread_exit(NULL);
	return NULL;
}

/**
 * Shutdown the ADNS thread.
 */
static void
mingw_adns_stop_thread(struct async_data *unused_data)
{
	(void) unused_data;
	mingw_adns_thread_run = FALSE;
}

static bool
mingw_adns_timer(void *unused_arg)
{
	struct async_data *ad = g_async_queue_try_pop(mingw_gtkg_main_async_queue);

	(void) unused_arg;
	
	if (NULL != ad) {
		if (common_dbg) {
			s_debug("performing callback to func @%p", ad->callback_func);
		}
		ad->callback_func(ad);
	} 

	return TRUE;		/* Keep calling */
}

bool
mingw_adns_send_request(const struct adns_request *req)
{
	if (req->common.reverse) {
		mingw_adns_getnameinfo(req);
	} else {
		mingw_adns_getaddrinfo(req);
	}
	return TRUE;
}

static bool mingw_adns_running;

void
mingw_adns_init(void)
{
	if (mingw_adns_running)
		return;

	/*
	 * Be extremely careful in the ADNS thread!
	 * gtk-gnutella was designed as mono-threaded application so its regular
	 * routines are NOT thread-safe.  Do NOT access any public functions or
	 * modify global variables from the ADNS thread!
	 *
	 * Dynamic memory allocation is possible via xmalloc(), walloc() and
	 * vmm_alloc() now that these allocators have been made thread-safe.
 	 */

	g_thread_init(NULL);
	mingw_gtkg_main_async_queue = g_async_queue_new();
	mingw_gtkg_adns_async_queue = g_async_queue_new();

	g_thread_create(mingw_adns_thread, NULL, FALSE, NULL);
	cq_periodic_main_add(1000, mingw_adns_timer, NULL);
	mingw_adns_running = TRUE;
}

void
mingw_adns_close(void)
{
	struct async_data *ad;

	if (!mingw_adns_running)
		return;

	/* Quit our ADNS thread */
	WALLOC0(ad);
	ad->thread_func = mingw_adns_stop_thread;

	g_async_queue_push(mingw_gtkg_adns_async_queue, ad);
	g_async_queue_unref(mingw_gtkg_adns_async_queue);
	g_async_queue_unref(mingw_gtkg_main_async_queue);
	mingw_adns_running = FALSE;
}

/*** End of ADNS section ***/

static const char *
mingw_get_folder_basepath(enum special_folder which_folder)
{
	const char *special_path = NULL;

	switch (which_folder) {
	case PRIVLIB_PATH:
		special_path = mingw_filename_nearby(
			"share" G_DIR_SEPARATOR_S PACKAGE);
		break;
	case NLS_PATH:
		special_path = mingw_filename_nearby(
			"share" G_DIR_SEPARATOR_S "locale");
		break;
	default:
		s_warning("%s() needs implementation for foldertype %d",
			G_STRFUNC, which_folder);
	}

	return special_path;
}

/**
 * Build pathname of file located nearby our executable.
 *
 * @return pointer to static data.
 */
const char *
mingw_filename_nearby(const char *filename)
{
	static char pathname[MAX_PATH_LEN];
	static wchar_t wpathname[MAX_PATH_LEN];
	static size_t offset;

	if ('\0' == pathname[0]) {
		bool error = FALSE;

		if (0 == GetModuleFileNameW(NULL, wpathname, sizeof wpathname)) {
			error = TRUE;
			errno = mingw_last_error();
			s_warning("cannot locate my executable: %m");
		} else {
			size_t conv = utf16_to_utf8(wpathname, pathname, sizeof pathname);
			if (conv > sizeof pathname) {
				error = TRUE;
				s_carp("%s: cannot convert UTF-16 path into UTF-8", G_STRFUNC);
			}
		}

		if (error)
			g_strlcpy(pathname, G_DIR_SEPARATOR_S, sizeof pathname);

		offset = filepath_basename(pathname) - pathname;

	}
	clamp_strcpy(&pathname[offset], sizeof pathname - offset, filename);

	return pathname;
}

/**
 * Check whether there is pending data for us to read on a pipe.
 */
static bool
mingw_fifo_pending(int fd)
{
	HANDLE h = (HANDLE) _get_osfhandle(fd);
	DWORD pending;

	if (INVALID_HANDLE_VALUE == h)
		return FALSE;

	if (0 == PeekNamedPipe(h, NULL, 0, NULL, &pending, NULL)) {
		errno = mingw_last_error();
		if (EPIPE == errno)
			return TRUE;		/* Let them read EOF */
		s_warning("peek failed for fd #%d: %m", fd);
		return FALSE;
	}

	return pending != 0;
}

/**
 * Check whether there is pending data for us to read on a tty / fifo stdin.
 */
bool
mingw_stdin_pending(bool fifo)
{
	return fifo ? mingw_fifo_pending(STDIN_FILENO) : booleanize(_kbhit());
}

/**
 * Get file ID.
 *
 * @return TRUE on success.
 */
static bool
mingw_get_file_id(const char *pathname, uint64 *id)
{
	HANDLE h;
	BY_HANDLE_FILE_INFORMATION fi;
	bool ok;
	pncs_t pncs;

	if (pncs_convert(&pncs, pathname))
		return FALSE;

	h = CreateFileW(pncs.utf16, 0,
			FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);

	if (INVALID_HANDLE_VALUE == h)
		return FALSE;

	ok = 0 != GetFileInformationByHandle(h, &fi);
	CloseHandle(h);

	if (!ok)
		return FALSE;

	*id = (uint64) fi.nFileIndexHigh << 32 | (uint64) fi.nFileIndexLow;

	return TRUE;
}

/**
 * Are the two files sharing the same file ID?
 */
bool
mingw_same_file_id(const char *pathname_a, const char *pathname_b)
{
	uint64 ia, ib;

	if (!mingw_get_file_id(pathname_a, &ia))
		return FALSE;

	if (!mingw_get_file_id(pathname_b, &ib))
		return FALSE;

	return ia == ib;
}

/**
 * Compute default gateway address.
 *
 * @param ip		where IPv4 gateway address is to be written
 *
 * @return 0 on success, -1 on failure with errno set.
 */
int
mingw_getgateway(uint32 *ip)
{
	MIB_IPFORWARDROW ipf;

	ZERO(&ipf);
	if (GetBestRoute(0, 0, &ipf) != NO_ERROR) {
		errno = mingw_last_error();
		return -1;
	}

	*ip = ntohl(ipf.dwForwardNextHop);
	return 0;
}

#ifdef EMULATE_GETTIMEOFDAY
/**
 * Get the current system time.
 *
 * @param tv	the structure to fill with the current time.
 * @param tz	(unused) normally a "struct timezone"
 */
int
mingw_gettimeofday(struct timeval *tv, void *tz)
{
	FILETIME ft;

	(void) tz;	/* We don't handle the timezone */

	GetSystemTimeAsFileTime(&ft);

	/*
	 * MSDN says that FILETIME contains a 64-bit value representing the number
	 * of 100-nanosecond intervals since January 1, 1601 (UTC).
	 *
	 * This is exactly 11644473600000000 usecs before the UNIX Epoch.
	 */

	mingw_filetime_to_timeval(&ft, tv, EPOCH_OFFSET);

	return 0;
}
#endif	/* EMULATE_GETTIMEOFDAY */

void mingw_vmm_post_init(void)
{
	s_info("VMM reserved %s of virtual space at [%p, %p]",
		compact_size(mingw_vmm.size, FALSE),
		mingw_vmm.reserved,
		ptr_add_offset(mingw_vmm.reserved, mingw_vmm.size));
	s_info("VMM left %s of virtual space unreserved",
		compact_size(mingw_vmm.later, FALSE));
}

void
mingw_init(void)
{
	WSADATA wsaData;

	if G_UNLIKELY(mingw_inited)
		return;

	mingw_inited = TRUE;

	if (WSAStartup(MAKEWORD(2,2), &wsaData) != NO_ERROR)
		s_error("WSAStartup() failed");
		
	libws2_32 = LoadLibrary(WS2_LIBRARY);
    if (libws2_32 != NULL) {
        WSAPoll = (WSAPoll_func_t) GetProcAddress(libws2_32, "WSAPoll");
    }
}

#ifdef MINGW_BACKTRACE_DEBUG
#define BACKTRACE_DEBUG(...)	s_minidbg(__VA_ARGS__)
#define mingw_backtrace_debug()	1
#else
#define BACKTRACE_DEBUG(...)
#define mingw_backtrace_debug()	0
#endif	/* MINGW_BACKTRACE_DEBUG */

#define MINGW_MAX_ROUTINE_LENGTH	0x2000
#define MINGW_FORWARD_SCAN			32
#define MINGW_SP_ALIGN				4
#define MINGW_SP_MASK				(MINGW_SP_ALIGN - 1)
#define MINGW_EMPTY_STACKFRAME		((void *) 1)

static inline bool
valid_ptr(const void * const p)
{
	ulong v = pointer_to_ulong(p);
	return v > 0x1000 && v < 0xfffff000 && mem_is_valid_ptr(p);
}

static inline bool
valid_stack_ptr(const void * const p, const void *top)
{
	ulong v = pointer_to_ulong(p);

	return 0 == (v & MINGW_SP_MASK) && vmm_is_stack_pointer(p, top);
}

/*
 * x86 leading instruction opcodes
 */
#define OPCODE_RET_NEAR		0xc3
#define OPCODE_RET_FAR		0xcb
#define OPCODE_RET_NEAR_POP	0xc2	/* Plus pop immediate 16-bit amount */
#define OPCODE_RET_FAR_POP	0xca	/* Plus pop immediate 16-bit amount */
#define OPCODE_NOP			0x90
#define OPCODE_CALL			0xe8
#define OPCODE_PUSH_EAX		0x50
#define OPCODE_PUSH_ECX		0x51
#define OPCODE_PUSH_EDX		0x52
#define OPCODE_PUSH_EBX		0x53
#define OPCODE_PUSH_ESP		0x54
#define OPCODE_PUSH_EBP		0x55
#define OPCODE_PUSH_ESI		0x56
#define OPCODE_PUSH_EDI		0x57
#define OPCODE_SUB_1		0x29	/* Substraction between registers */
#define OPCODE_SUB_2		0x81	/* Need further probing for real opcode */
#define OPCODE_SUB_3		0x83	/* Need further probing for real opcode */
#define OPCODE_MOV_REG		0x89	/* Move one register to another */
#define OPCODE_MOV_IMM_EAX	0xb8	/* Move immediate value to register EAX */
#define OPCODE_MOV_IMM_ECX	0xb9
#define OPCODE_MOV_IMM_EDX	0xba
#define OPCODE_MOV_IMM_EBX	0xbb
#define OPCODE_MOV_IMM_ESP	0xbc
#define OPCODE_MOV_IMM_EBP	0xbd
#define OPCODE_MOV_IMM_ESI	0xbe
#define OPCODE_MOV_IMM_EDI	0xbf
#define OPCODE_JMP_SHORT	0xeb	/* Followed by signed byte */
#define OPCODE_JMP_LONG		0xe9	/* Followed by signed 32-bit value */
#define OPCODE_LEA			0x8d
#define OPCODE_XOR_1		0x31	/* Between registers if mod=3 */
#define OPCODE_XOR_2		0x33	/* Complex XOR involving memory */
#define OPCODE_NONE_1		0x26	/* Not a valid opcode */
#define OPCODE_NONE_2		0x2e
#define OPCODE_NONE_3		0x36
#define OPCODE_NONE_4		0x3E
#define OPCODE_NONE_5		0x64
#define OPCODE_NONE_6		0x65
#define OPCODE_NONE_7		0x66
#define OPCODE_NONE_8		0x67

/*
 * x86 follow-up instruction parsing
 */
#define OPMODE_MODE_MASK	0xc0	/* Mask to get a instruction mode code */
#define OPMODE_OPCODE		0x38	/* Mask to extract extra opcode info */
#define OPMODE_REG_SRC_MASK	0x38	/* Mask to extract source register */
#define OPMODE_REG_DST_MASK	0x07	/* Mask to extract destination register */
#define OPMODE_SUB			5		/* Extra opcode indicating a SUB */
#define OPMODE_SUB_ESP		0xec	/* Byte after leading opcode for SUB ESP */
#define OPMODE_REG_ESP_EBP	0xe5	/* Byte after MOVL to move ESP to EBP */

/*
 * x86 register numbers, as encoded in instructions.
 */
#define OPREG_EAX			0
#define OPREG_ECX			1
#define OPREG_EDX			2
#define OPREG_EBX			3
#define OPREG_ESP			4
#define OPREG_EBP			5
#define OPREG_ESI			6
#define OPREG_EDI			7

static inline uint8
mingw_op_mod_code(uint8 mbyte)
{
	return (mbyte & OPMODE_MODE_MASK) >> 6;
}

static inline uint8
mingw_op_src_register(uint8 mbyte)
{
	return (mbyte & OPMODE_REG_SRC_MASK) >> 3;
}

static inline uint8
mingw_op_dst_register(uint8 mbyte)
{
	return mbyte & OPMODE_REG_DST_MASK;
}

#define MINGW_TEXT_OFFSET	0x1000	/* Text offset after mapping base */

#define MINGW_ROUTINE_ALIGN	4
#define MINGW_ROUTINE_MASK	(MINGW_ROUTINE_ALIGN - 1)

#define mingw_routine_align(x) ulong_to_pointer( \
	(pointer_to_ulong(x) + MINGW_ROUTINE_MASK) & ~MINGW_ROUTINE_MASK)

/**
 * Expected unwinding stack frame, if present and maintained by routines.
 */
struct stackframe {
	struct stackframe *next;
	void *ret;
};

#ifdef MINGW_BACKTRACE_DEBUG
/**
 * @return opcode leading mnemonic string.
 */
static const char *
mingw_opcode_name(uint8 opcode)
{
	switch (opcode) {
	case OPCODE_RET_NEAR:
	case OPCODE_RET_FAR:
	case OPCODE_RET_NEAR_POP:
	case OPCODE_RET_FAR_POP:
		return "RET";
	case OPCODE_NOP:
		return "NOP";
	case OPCODE_CALL:
		return "CALL";
	case OPCODE_PUSH_EAX:
	case OPCODE_PUSH_EBX:
	case OPCODE_PUSH_ECX:
	case OPCODE_PUSH_EDX:
	case OPCODE_PUSH_ESP:
	case OPCODE_PUSH_EBP:
	case OPCODE_PUSH_ESI:
	case OPCODE_PUSH_EDI:
		return "PUSH";
	case OPCODE_MOV_REG:
	case OPCODE_MOV_IMM_EAX:
	case OPCODE_MOV_IMM_EBX:
	case OPCODE_MOV_IMM_ECX:
	case OPCODE_MOV_IMM_EDX:
	case OPCODE_MOV_IMM_ESP:
	case OPCODE_MOV_IMM_EBP:
	case OPCODE_MOV_IMM_ESI:
	case OPCODE_MOV_IMM_EDI:
		return "MOV";
	case OPCODE_JMP_SHORT:
	case OPCODE_JMP_LONG:
		return "JMP";
	case OPCODE_LEA:
		return "LEA";
	case OPCODE_XOR_1:
	case OPCODE_XOR_2:
		return "XOR";
	case OPCODE_NONE_1:
	case OPCODE_NONE_2:
	case OPCODE_NONE_3:
	case OPCODE_NONE_4:
	case OPCODE_NONE_5:
	case OPCODE_NONE_6:
	case OPCODE_NONE_7:
	case OPCODE_NONE_8:
	default:
		return "?";
	}
}
#endif /* MINGW_BACKTRACE_DEBUG */

/**
 * Is the SUB opcode pointed at by ``op'' targetting ESP?
 */
static bool
mingw_opcode_is_sub_esp(const uint8 *op)
{
	const uint8 *p = op;
	uint8 mbyte = p[1];

	BACKTRACE_DEBUG("%s: op=0x%x, next=0x%x", G_STRFUNC, *op, mbyte);

	switch (*op) {
	case OPCODE_SUB_1:
		return OPREG_ESP == mingw_op_dst_register(mbyte);
	case OPCODE_SUB_2:
	case OPCODE_SUB_3:
		{
			uint8 code = mingw_op_src_register(mbyte);
			uint8 mode = mingw_op_mod_code(mbyte);
			if (code != OPMODE_SUB || mode != 3)
				return FALSE;	/* Not a SUB opcode targeting a register */
			return OPREG_ESP == mingw_op_dst_register(mbyte);
		}
	}

	g_assert_not_reached();
}

/**
 * Scan forward looking for one of the SUB instructions that can substract
 * a value from the ESP register.
 *
 * This can be one of (Intel notation):
 *
 * 		SUB ESP, <value>		; short stack reserve
 * 		SUB ESP, EAX			; large stack reserve
 *
 * @param start		initial program counter
 * @param max		absolute maximum PC value
 * @param at_start		known to be at the starting point of the routine
 * @param has_frame	set to TRUE if we saw a frame linking at the beginning
 * @param savings	indicates leading register savings done by the routine
 *
 * @return pointer to the start of the SUB instruction, NULL if we can't
 * find it, meaning the starting point was probably not the start of
 * a routine, MINGW_EMPTY_STACKFRAME if there is no SUB instruction.
 */
static const void *
mingw_find_esp_subtract(const void *start, const void *max, bool at_start,
	bool *has_frame, size_t *savings)
{
	const void *maxscan;
	const uint8 *p = start;
	const uint8 *first_opcode = p;
	bool saved_ebp = FALSE;
	size_t pushes = 0;

	maxscan = const_ptr_add_offset(start, MINGW_FORWARD_SCAN);
	if (ptr_cmp(maxscan, max) > 0)
		maxscan = max;

	if (mingw_backtrace_debug()) {
		s_minidbg("%s: next %zu bytes after pc=%p%s",
			G_STRFUNC, 1 + ptr_diff(maxscan, p),
			p, at_start ? " (known start)" : "");
		dump_hex(stderr, "", p, 1 + ptr_diff(maxscan, p));
	}

	for (p = start; ptr_cmp(p, maxscan) <= 0; p++) {
		const void *window;
		uint8 op;
		unsigned fill = 0;

		switch ((op = *p)) {
		case OPCODE_NONE_1:
		case OPCODE_NONE_2:
		case OPCODE_NONE_3:
		case OPCODE_NONE_4:
		case OPCODE_NONE_5:
		case OPCODE_NONE_6:
		case OPCODE_NONE_7:
		case OPCODE_NONE_8:
		case OPCODE_NOP:
			fill = 1;
			goto filler;
		case OPCODE_LEA:
			/*
			 * Need to decode further to know how many bytes are taken
			 * by this versatile instruction.
			 */
			{
				uint8 mode = mingw_op_mod_code(p[1]);
				uint8 reg = mingw_op_dst_register(p[1]);
				switch (mode) {
				case 0:
					/*
					 * ``reg'' encodes the following:
					 *
					 * 4 = [sib] (32-bit SIB Byte follows)
					 * 5 = disp32
					 * others = register
					 */

					if (4 == reg) {
						fill = 3;
					} if (5 == reg) {
						fill = 6;
					} else {
						fill = 2;
					}
					goto filler;
				case 1:
					/*
					 * ``reg'' encodes the following:
					 *
					 * 4 = [sib] + disp8
					 * others = register + disp8
					 */

					if (4 == reg) {
						fill = 4;
					} else {
						fill = 3;
					}
					goto filler;
				case 2:
					/*
					 * ``reg'' encodes the following:
					 *
					 * 4 = [sib] + disp32
					 * others = register + disp32
					 */
					if (4 == reg) {
						fill = 7;
					} else {
						fill = 6;
					}
					goto filler;
				case 3:
					fill = 2;
					goto filler;
				default:
					g_assert_not_reached();
				}
			}
			break;
		case OPCODE_PUSH_EBP:
			/*
			 * The frame pointer is saved if the routine begins with (Intel
			 * notation):
			 *
			 *	PUSH EBP
			 *  MOV  EBP, ESP
			 *
			 * to create the frame pointer link.
			 */
			first_opcode = p + 1;	/* Expects the MOV operation to follow */
			/* FALL THROUGH */
		case OPCODE_PUSH_EAX:
		case OPCODE_PUSH_EBX:
		case OPCODE_PUSH_ECX:
		case OPCODE_PUSH_EDX:
		case OPCODE_PUSH_ESP:
		case OPCODE_PUSH_ESI:
		case OPCODE_PUSH_EDI:
			pushes++;
			break;
		case OPCODE_MOV_IMM_EAX:
		case OPCODE_MOV_IMM_EBX:
		case OPCODE_MOV_IMM_ECX:
		case OPCODE_MOV_IMM_EDX:
		case OPCODE_MOV_IMM_ESP:
		case OPCODE_MOV_IMM_EBP:
		case OPCODE_MOV_IMM_ESI:
		case OPCODE_MOV_IMM_EDI:
			p += 4;				/* Skip immediate value */
			break;
		case OPCODE_MOV_REG:
			if (OPMODE_REG_ESP_EBP == p[1])
				saved_ebp = p == first_opcode;
			p += 1;				/* Skip mode byte */
			break;
		case OPCODE_CALL:
			/* Stackframe link created, no stack adjustment */
			if (saved_ebp)
				return MINGW_EMPTY_STACKFRAME;
			p += 4;				/* Skip offset */
			break;
		case OPCODE_XOR_1:
			if (OPMODE_MODE_MASK == (OPMODE_MODE_MASK & p[1])) {
				/* XOR between registers, same register to zero it */
				uint8 operands = p[1];
				uint8 reg1 = mingw_op_src_register(operands);
				uint8 reg2 = mingw_op_dst_register(operands);
				if (reg1 == reg2) {
					p += 1;
					break;
				}
			}
			/* XOR REG, REG is the only instruction we allow in the prologue */
			return NULL;
		case OPCODE_SUB_1:
		case OPCODE_SUB_2:
		case OPCODE_SUB_3:
			if (mingw_opcode_is_sub_esp(p)) {
				*has_frame = saved_ebp;
				*savings = pushes;
				return p;
			}
			switch (*p) {
			case OPCODE_SUB_1:
				p += 1;
				break;
			case OPCODE_SUB_2:
				p += 5;
				break;
			case OPCODE_SUB_3:
				p += 2;
				break;
			}
			break;
		default:
			/*
			 * If we're not on an aligned routine starting point, assume
			 * this is part of a filling instruction and ignore, provided
			 * we haven't seen any PUSH yet.
			 */
			if (0 == pushes && !at_start && p != mingw_routine_align(p)) {
				fill = 1;
				goto filler;
			}
			return NULL;
		}

		continue;

	filler:
		/*
		 * Handle "filling" instructions between last RET / JMP and
		 * the next routine  Move the scanning window forward to avoid
		 * counting filling instructions.
		 */

		BACKTRACE_DEBUG("%s: ignoring %s filler (%u byte%s) at %p", G_STRFUNC,
			mingw_opcode_name(op), fill, 1 == fill ? "" : "s", p);

		first_opcode = p + fill;
		p += (fill - 1);
		window = const_ptr_add_offset(maxscan, fill);
		if (ptr_cmp(window, max) <= 0)
			maxscan = window;
	}

	return NULL;
}

/**
 * Parse beginning of routine to know how many registers are saved, whether
 * there is a leading frame being formed, and how large the stack is.
 *
 * @param pc			starting point
 * @param max			maximum PC we accept to scan forward
 * @param at_start		known to be at the starting point of the routine
 * @param has_frame		set to TRUE if we saw a frame linking at the beginning
 * @param savings		indicates leading register savings done by the routine
 * @param offset		computed stack offsetting
 *
 * @return TRUE if ``pc'' pointed to a recognized function prologue.
 */
static bool
mingw_analyze_prologue(const void *pc, const void *max, bool at_start,
	bool *has_frame, size_t *savings, unsigned *offset)
{
	const uint8 *sub;

	if (ptr_cmp(pc, max) >= 0)
		return FALSE;

	sub = mingw_find_esp_subtract(pc, max, at_start, has_frame, savings);

	if (MINGW_EMPTY_STACKFRAME == sub) {
		BACKTRACE_DEBUG("%s: no SUB operation at pc=%p, %s frame",
			G_STRFUNC, pc, *has_frame ? "with" : "no");
		*offset = 0;
		return TRUE;
	} else if (sub != NULL) {
		uint8 op;

		BACKTRACE_DEBUG("%s: found SUB operation at "
			"pc=%p, opcode=0x%x, mod=0x%x, %s frame",
			G_STRFUNC, sub, sub[0], sub[1], *has_frame ? "with" : "no");

		switch (*sub) {
		case OPCODE_SUB_1:
			/*
			 * This is the pattern used by gcc for large stacks.
			 *
			 * (Note this uses AT&T syntax, not the Intel one, so
			 * order is source, destination as opposed to the regular
			 * Intel convention)
			 *
			 *    movl    $65564, %eax
			 *    call    ___chkstk_ms
			 *    subl    %eax, %esp
			 *
			 * We found the last instruction, we need to move back 10
			 * bytes to reach the MOV instruction
			 */

			op = *(sub - 10);
			if (op != OPCODE_MOV_IMM_EAX)
				return FALSE;

			*offset = peek_le32(sub + 1);
			return TRUE;
		case OPCODE_SUB_2:
			/* subl    $220, %esp */
			g_assert(OPMODE_SUB_ESP == sub[1]);
			*offset = peek_le32(sub + 2);
			return TRUE;
		case OPCODE_SUB_3:
			/* subl    $28, %esp */
			g_assert(OPMODE_SUB_ESP == sub[1]);
			*offset = peek_u8(sub + 2);
			return TRUE;
		}
		g_assert_not_reached();
	}

	return FALSE;
}

/**
 * Intuit return address given current PC and SP.
 *
 * Uses black magic: disassembles the code on the fly knowing the gcc
 * initial function patterns to look for the instruction that alters the SP.
 *
 * @attention
 * This is not perfect and based on heuristics.  Changes in the compiler
 * generation pattern may break this routine.  Moreover, backtracing through
 * a routine using alloca() will not work because the initial stack reserve is
 * later altered, so the stack pointer in any routine that it calls will be
 * perturbed and will not allow correct reading of the return address.
 *
 * @param next_pc		where next PC is written
 * @param next_sp		where next SP is written
 * @param next_sf		where next SF is written, NULL if none seen
 *
 * @return TRUE if we were able to recognize the start of the routine and
 * compute the proper stack offset, FALSE otherwise.
 */
static bool
mingw_get_return_address(const void **next_pc, const void **next_sp,
	const void **next_sf)
{
	const void *pc = *next_pc;
	const void *sp = *next_sp;
	const uint8 *p;
	unsigned offset = 0;
	bool has_frame = FALSE;
	size_t savings = 0;

	BACKTRACE_DEBUG("%s: pc=%p, sp=%p", G_STRFUNC, pc, sp);

	/*
	 * If we can determine the start of the routine, get there first.
	 */

	p = stacktrace_routine_start(pc);

	if (p != NULL && valid_ptr(p)) {
		BACKTRACE_DEBUG("%s: known routine start for pc=%p is %p (%s)",
			G_STRFUNC, pc, p, stacktrace_routine_name(p, TRUE));

		if (mingw_analyze_prologue(p, pc, TRUE, &has_frame, &savings, &offset))
			goto found_offset;

		BACKTRACE_DEBUG("%s: %p does not seem to be a valid prologue, scanning",
			G_STRFUNC, p);
	} else {
		BACKTRACE_DEBUG("%s: pc=%p falls in %s from %s", G_STRFUNC, pc,
			stacktrace_routine_name(pc, TRUE), dl_util_get_path(pc));
	}

	/*
	 * Scan backwards to find a previous RET / JMP instruction.
	 */

	for (p = pc; ptr_diff(pc, p) < MINGW_MAX_ROUTINE_LENGTH; /* empty */) {
		uint8 op;
		
		const uint8 *next;

		if (!valid_ptr(p))
			return FALSE;

		switch ((op = *p)) {
		case OPCODE_RET_NEAR:
		case OPCODE_RET_FAR:
			next = p + 1;
			break;
		case OPCODE_RET_NEAR_POP:
		case OPCODE_RET_FAR_POP:
			next = p + 3;	/* Skip next immediate 16-bit offset */
			break;
		case OPCODE_JMP_SHORT:
			next = p + 2;	/* Skip 8-bit offset */
			break;
		case OPCODE_JMP_LONG:
			next = p + 5;	/* Skip 32-bit target */
			break;
		default:
			goto next;
		}

		BACKTRACE_DEBUG("%s: found %s operation at pc=%p, opcode=0x%x",
			G_STRFUNC, mingw_opcode_name(op), p, op);

		/*
		 * Could have found a byte that is part of a longer opcode, since
		 * the x86 has variable-length instructions.
		 *
		 * Scan forward for a SUB instruction targetting the ESP register.
		 */

		if (
			mingw_analyze_prologue(next, pc, FALSE,
				&has_frame, &savings, &offset)
		)
			goto found_offset;

	next:
		p--;
	}

	return FALSE;

found_offset:
	g_assert(0 == (offset & 3));	/* Multiple of 4 */

	BACKTRACE_DEBUG("%s: offset = %u, %zu leading push%s",
		G_STRFUNC, offset, savings, 1 == savings ? "" : "es");

	/*
	 * We found that the current routine decreased the stack pointer by
	 * ``offset'' bytes upon entry.  It is expected to increase the stack
	 * pointer by the same amount before returning, at which time it will
	 * pop from the stack the return address.
	 *
	 * This is what we're computing now, to find out the return address
	 * that is on the stack.
	 *
	 * Once it pops the return address, the processor will also increase the
	 * stack pointer by 4 bytes, so this will be the value of ESP upon return.
	 *
	 * Moreover, if we have seen a "PUSH EBP; MOV EBP, ESP" sequence at the
	 * beginning, then the stack frame pointer was maintained by the callee.
	 * In AT&T syntax (which reverses the order of arguments compared to the
	 * Intel notation, becoming source, destination) used by gas, that would be:
	 *
	 *     pushl   %ebp
	 *     movl    %esp, %ebp           ; frame linking now established
	 *     subl    $56, %esp            ; reserve 56 bytes on the stack
	 */

	offset += 4 * savings;
	sp = const_ptr_add_offset(sp, offset);

	if (has_frame) {
		const void *sf, *fp;
		g_assert(savings >= 1);
		sf = const_ptr_add_offset(sp, -4);
		fp = ulong_to_pointer(peek_le32(sf));
		if (ptr_cmp(fp, sp) <= 0) {
			BACKTRACE_DEBUG("%s: inconsistent fp %p (\"above\" sp %p)",
				G_STRFUNC, fp, sp);
			has_frame = FALSE;
		} else if (!vmm_is_stack_pointer(fp, sf)) {
			BACKTRACE_DEBUG("%s: invalid fp %p (not a stack pointer)",
				G_STRFUNC, fp);
			has_frame = FALSE;
		}
		*next_sf = has_frame ? sf : NULL;
	} else {
		*next_sf = NULL;
	}

	*next_pc = ulong_to_pointer(peek_le32(sp));	/* Pushed return address */

	if (!valid_ptr(*next_pc))
		return FALSE;

	*next_sp = const_ptr_add_offset(sp, 4);	/* After popping return address */

	if (mingw_backtrace_debug() && has_frame) {
		const struct stackframe *sf = *next_sf;
		s_minidbg("%s: next frame at %p "
			"(contains next=%p, ra=%p), computed ra=%p",
			G_STRFUNC, sf, sf->next, sf->ret, *next_pc);
	}

	return TRUE;
}

/**
 * Unwind the stack, using the saved context to gather the initial program
 * counter, stack pointer and stack frame pointer.
 *
 * @param buffer	where function addresses are written to
 * @param size		amount of entries in supplied buffer
 * @param c			saved CPU context
 * @param offset	topmost frames to skip
 */
static int
mingw_stack_unwind(void **buffer, int size, CONTEXT *c, int skip)
{
	int i = 0;
	const struct stackframe *sf;
	const void *sp, *pc, *top;

	/*
	 * We used to rely on StackWalk() here, but we no longer do because
	 * it did not work well and failed to provide useful stacktraces.
	 *
	 * Neither does blind following of frame pointers because some routines
	 * simply do not bother to maintain the frame pointers, especially
	 * those known by gcc as being non-returning routines such as
	 * assertion_abort(). Plain stack frame following could not unwind
	 * past that.
	 *
	 * Since it is critical on Windows to obtain a somewhat meaningful
	 * stack frame to be able to debug anything, given the absence of
	 * core dumps (for post-mortem analysis) and of fork() (for launching
	 * a debugger to obtain the stack trace), extraordinary measures were
	 * called for...
	 *
	 * Therefore, we now perform our own unwinding which does not rely on
	 * plain stack frame pointer following but rather (minimally)
	 * disassembles the routine prologues to find out how many stack
	 * space is used by each routine, so that we can find where the
	 * caller pushed the return address on the stack.
	 *
	 * When the start of a routine is not known, the code attempts to
	 * guess where it may be by scanning backwards until it finds what
	 * is probably the end of the previous routine.  Since the x86 is a
	 * CISC machine with a variable-length instruction set, this operation
	 * cannot be entirely fool-proof, since the opcodes used for RET or
	 * JMP instructions could well be actually parts of immediate operands
	 * given to some other instruction.
	 *
	 * Hence there is logic to determine whether the initial starting point
	 * is actually a valid routine prologue, relying on what we know gcc
	 * can use before it adjusts the stack pointer.
	 *
	 * Despite being a hack because it is based on known routine generation
	 * patterns from gcc, it works surprisingly well and, in any case, is
	 * far more useful than the original code that used StackWalk(), or
	 * the simple gcc unwinding which merely follows frame pointers.
	 *		--RAM, 2012-03-19
	 */

	sf = ulong_to_pointer(c->Ebp);
	sp = ulong_to_pointer(c->Esp);
	pc = ulong_to_pointer(c->Eip);

	BACKTRACE_DEBUG("%s: pc=%p, sf=%p, sp=%p [skip %d]",
		G_STRFUNC, pc, sf, sp, skip);

	if (0 == skip--)
		buffer[i++] = deconstify_pointer(pc);

	if (!valid_stack_ptr(sp, sp))
		goto done;

	top = sp;

	while (i < size) {
		const void *next = NULL;

		BACKTRACE_DEBUG("%s: i=%d, sp=%p, sf=%p, pc=%p",
			G_STRFUNC, i, sp, sf, pc);

		if (!valid_ptr(pc) || !valid_stack_ptr(sp, top))
			break;

		if (!valid_stack_ptr(sf, top) || ptr_cmp(sf, sp) <= 0)
			sf = NULL;

		if (!mingw_get_return_address(&pc, &sp, &next)) {
			if (sf != NULL) {
				BACKTRACE_DEBUG("%s: trying to follow sf=%p", G_STRFUNC, sf);
				next = sf->next;

				if (!valid_ptr(sf->ret))
					break;

				pc = sf->ret;
				sp = &sf[1];	/* After popping returned value */

				BACKTRACE_DEBUG("%s: following frame: "
					"next sf=%p, pc=%p, rebuilt sp=%p",
					G_STRFUNC, next, pc, sp);

				if (!valid_stack_ptr(next, top) || ptr_cmp(next, sf) <= 0)
					next = NULL;
			} else {
				BACKTRACE_DEBUG("%s: out of frames", G_STRFUNC);
				break;
			}
		} else {
			int d;

			BACKTRACE_DEBUG("%s: intuited next pc=%p, sp=%p, "
				"rebuilt sf=%p [old sf=%p]",
				G_STRFUNC, pc, sp, next, sf);

			/*
			 * Leave frame pointer intact if the stack pointer is still
			 * smaller than the last frame pointer: it means the routine
			 * that we backtraced through did not save a frame pointer, so
			 * it would have been invisible if we had followed the frame
			 * pointer.
			 */

			d = (NULL == sf) ? 0 : ptr_cmp(sp, sf);

			if (d < 0) {
				BACKTRACE_DEBUG("%s: keeping old sf=%p, since sp=%p",
					G_STRFUNC, sf, sp);
				next = sf;
			} else if (d > 0) {
				if (sp == &sf[1]) {
					BACKTRACE_DEBUG("%s: reached sf=%p at sp=%p, "
						"next sf=%p, current ra=%p",
						G_STRFUNC, sf, sp, sf->next, sf->ret);
					if (NULL == next && valid_stack_ptr(sf->next, top))
						next = sf->next;
				}
			}
		}

		if (skip-- <= 0)
			buffer[i++] = deconstify_pointer(pc);

		sf = next;
	}

done:
	BACKTRACE_DEBUG("%s: returning %d", G_STRFUNC, i);

	return i;
}

/**
 * Fill supplied buffer with current stack, skipping the topmost frames.
 *
 * @param buffer	where function addresses are written to
 * @param size		amount of entries in supplied buffer
 * @param offset	topmost frames to skip
 *
 * @return amount of entries written into buffer[].
 */
int
mingw_backtrace(void **buffer, int size, size_t offset)
{
	CONTEXT c;
	HANDLE thread;

	thread = GetCurrentThread();

	ZERO(&c);
	c.ContextFlags = CONTEXT_FULL;

	/*
	 * We'd need RtlCaptureContext() but it's not avaialable through MinGW.
	 *
	 * Although MSDN says the context will be corrupted, we're not doing
	 * context-switching here.  What's important is that the stack addresses
	 * be filled, and experience shows they are properly filled in.
	 */

	GetThreadContext(thread, &c);

	return mingw_stack_unwind(buffer, size, &c, offset);
}

#ifdef EMULATE_DLADDR
static int mingw_dl_error;

/**
 * Return a human readable string describing the most recent error
 * that occurred.
 */
const char *
mingw_dlerror(void)
{
	return g_strerror(mingw_dl_error);
}

/**
 * Emulates linux's dladdr() routine.
 *
 * Given a function pointer, try to resolve name and file where it is located.
 *
 * If no symbol matching addr could be found, then dli_sname and dli_saddr are
 * set to NULL.
 *
 * @param addr		pointer within function
 * @param info		where results are returned
 *
 * @return 0 on error, non-zero on success.
 */
int
mingw_dladdr(void *addr, Dl_info *info)
{
	static time_t last_init;
	static char path[MAX_PATH_LEN];
	static wchar_t wpath[MAX_PATH_LEN];
	static char buffer[sizeof(IMAGEHLP_SYMBOL) + 256];
	time_t now;
	HANDLE process = NULL;
	IMAGEHLP_SYMBOL *symbol = (IMAGEHLP_SYMBOL *) buffer;
	DWORD disp = 0;

	/*
	 * Do not issue a SymInitialize() too often, yet let us do one from time
	 * to time in case we loaded a new DLL since last time.
	 */

	now = tm_time();

	if (0 == last_init || delta_time(now, last_init) > 5) {
		static bool initialized;

		process = GetCurrentProcess();

		if (initialized)
			SymCleanup(process);

		if (!SymInitialize(process, 0, TRUE)) {
			initialized = FALSE;
			mingw_dl_error = GetLastError();
			s_warning("SymInitialize() failed: error = %d (%s)",
				mingw_dl_error, mingw_dlerror());
		} else {
			initialized = TRUE;
			mingw_dl_error = 0;
		}

		last_init = now;
	}

	ZERO(info);

	if (0 != mingw_dl_error)
		return 0;		/* Signals error */

	if (NULL == addr)
		return 1;		/* OK */

	if (NULL == process)
		process = GetCurrentProcess();

	info->dli_fbase = ulong_to_pointer(
		SymGetModuleBase(process, pointer_to_ulong(addr)));
	
	if (NULL == info->dli_fbase) {
		mingw_dl_error = GetLastError();
		return 0;		/* Unknown, error */
	}

	if (GetModuleFileNameW((HINSTANCE) info->dli_fbase, wpath, sizeof wpath)) {
		size_t conv = utf16_to_utf8(wpath, path, sizeof path);
		if (conv <= sizeof path)
			info->dli_fname = path;
	}

	symbol->SizeOfStruct = sizeof buffer;
	symbol->MaxNameLength = 255;

	if (SymGetSymFromAddr(process, pointer_to_ulong(addr), &disp, symbol)) {
		info->dli_sname = symbol->Name;
		info->dli_saddr = ptr_add_offset(addr, -disp);
	}

	/*
	 * Windows offsets the actual loading of the text by MINGW_TEXT_OFFSET
	 * bytes, as determined empirically.
	 */

	info->dli_fbase = ptr_add_offset(info->dli_fbase, MINGW_TEXT_OFFSET);

	return 1;			/* OK */
}
#endif	/* EMULATE_DLADDR */

/**
 * Convert exception code to string.
 */
static G_GNUC_COLD const char *
mingw_exception_to_string(int code)
{
	switch (code) {
	case EXCEPTION_BREAKPOINT:				return "Breakpoint";
	case EXCEPTION_SINGLE_STEP:				return "Single step";
	case EXCEPTION_STACK_OVERFLOW:			return "Stack overflow";
	case EXCEPTION_ACCESS_VIOLATION:		return "Access violation";
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:	return "Array bounds exceeded";
	case EXCEPTION_IN_PAGE_ERROR:			return "Paging error";
	case EXCEPTION_DATATYPE_MISALIGNMENT:	return "Bus error";
	case EXCEPTION_FLT_DENORMAL_OPERAND:	return "Float denormal operand";
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:		return "Float divide by zero";
	case EXCEPTION_FLT_INEXACT_RESULT:		return "Float inexact result";
	case EXCEPTION_FLT_INVALID_OPERATION:	return "Float invalid operation";
	case EXCEPTION_FLT_OVERFLOW:			return "Float overflow";
	case EXCEPTION_FLT_STACK_CHECK:			return "Float stack check";
	case EXCEPTION_FLT_UNDERFLOW:			return "Float underflow";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:		return "Integer divide by zero";
	case EXCEPTION_INT_OVERFLOW:			return "Integer overflow";
	case EXCEPTION_ILLEGAL_INSTRUCTION:		return "Illegal instruction";
	case EXCEPTION_PRIV_INSTRUCTION:		return "Privileged instruction";
	case EXCEPTION_NONCONTINUABLE_EXCEPTION:return "Continued after exception";
	case EXCEPTION_INVALID_DISPOSITION:		return "Invalid disposition";
	default:								return "Unknown exception";
	}
}

/**
 * Log reported exception.
 */
static G_GNUC_COLD void
mingw_exception_log(int code, const void *pc)
{
	DECLARE_STR(11);
	char time_buf[18];
	const char *name;
	const char *file = NULL;

	crash_time(time_buf, sizeof time_buf);
	name = stacktrace_routine_name(pc, TRUE);
	if (is_strprefix(name, "0x"))
		name = NULL;

	if (!stacktrace_pc_within_our_text(pc))
		file = dl_util_get_path(pc);

	print_str(time_buf);										/* 0 */
	print_str(" (CRITICAL): received exception at PC=0x");		/* 1 */
	print_str(pointer_to_string(pc));							/* 2 */
	if (name != NULL) {
		print_str(" (");										/* 3 */
		print_str(name);										/* 4 */
		print_str(")");											/* 5 */
	}
	if (file != NULL) {
		print_str(" from ");									/* 6 */
		print_str(file);										/* 7 */
	}
	print_str(": ");											/* 8 */
	print_str(mingw_exception_to_string(code));					/* 9 */
	print_str("\n");											/* 10 */

	flush_err_str();
	if (log_stdout_is_distinct())
		flush_str(STDOUT_FILENO);

	/*
	 * Format an error message to propagate into the crash log.
	 */

	{
		char data[256];
		str_t s;

		str_new_buffer(&s, data, 0, sizeof data);
		str_printf(&s, "%s at PC=%p", mingw_exception_to_string(code), pc);

		if (name != NULL)
			str_catf(&s, " (%s)", name);

		if (file != NULL)
			str_catf(&s, " from %s", file);

		crash_set_error(str_2c(&s));
	}
}

/**
 * Log extra information on memory faults.
 */
static G_GNUC_COLD void
mingw_memory_fault_log(const EXCEPTION_RECORD *er)
{
	DECLARE_STR(6);
	char time_buf[18];
	const char *prot = "unknown";
	const void *va = NULL;

	if (er->NumberParameters >= 2) {
		switch (er->ExceptionInformation[0]) {
		case 0:		prot = "read"; break;
		case 1:		prot = "write"; break;
		case 8:		prot = "execute"; break;
		}
		va = ulong_to_pointer(er->ExceptionInformation[1]);
	}

	crash_time(time_buf, sizeof time_buf);

	print_str(time_buf);							/* 0 */
	print_str(" (CRITICAL): memory fault (");		/* 1 */
	print_str(prot);								/* 2 */
	print_str(") at VA=0x");						/* 3 */
	print_str(pointer_to_string(va));				/* 4 */
	print_str("\n");								/* 5 */

	flush_err_str();
	if (log_stdout_is_distinct())
		flush_str(STDOUT_FILENO);

	/*
	 * Format an additional error message to propagate into the crash log.
	 */

	{
		char data[80];

		str_bprintf(data, sizeof data, "; %s fault at VA=%p", prot, va);
		crash_append_error(data);
	}
}

static volatile sig_atomic_t in_exception_handler;
static void *mingw_stack[STACKTRACE_DEPTH_MAX];

bool
mingw_in_exception(void)
{
	return in_exception_handler;
}

/**
 * Our default exception handler.
 */
static G_GNUC_COLD LONG WINAPI
mingw_exception(EXCEPTION_POINTERS *ei)
{
	EXCEPTION_RECORD *er;
	int signo = 0;

	in_exception_handler++;		/* Will never be reset, we're crashing */
	er = ei->ExceptionRecord;

	/*
	 * Don't use too much stack if we're facing a stack overflow.
	 * We'll emit a short message below in that case.
	 *
	 * However, apparently the exceptions are delivered on a distinct stack.
	 * It may be very samll, for all we know, so still be cautious.
	 */

	if (EXCEPTION_STACK_OVERFLOW != er->ExceptionCode)
		mingw_exception_log(er->ExceptionCode, er->ExceptionAddress);

	switch (er->ExceptionCode) {
	case EXCEPTION_BREAKPOINT:
	case EXCEPTION_SINGLE_STEP:
		signo = SIGTRAP;
		break;
	case EXCEPTION_STACK_OVERFLOW:
		/*
		 * With a stack overflow, we may not be able to continue very
		 * far, so log the fact as soon as possible.
		 */
		{
			DECLARE_STR(1);

			print_str("Got stack overflow -- crashing.\n");
			flush_err_str();
			if (log_stdout_is_distinct())
				flush_str(STDOUT_FILENO);
		}
		signo = SIGSEGV;
		break;
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_IN_PAGE_ERROR:
		mingw_memory_fault_log(er);
		/* FALL THROUGH */
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		signo = SIGSEGV;
		break;
	case EXCEPTION_DATATYPE_MISALIGNMENT:
		signo = SIGBUS;
		break;
	case EXCEPTION_FLT_DENORMAL_OPERAND:
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
	case EXCEPTION_FLT_INEXACT_RESULT:
	case EXCEPTION_FLT_INVALID_OPERATION:
	case EXCEPTION_FLT_OVERFLOW:
	case EXCEPTION_FLT_STACK_CHECK:
	case EXCEPTION_FLT_UNDERFLOW:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_INT_OVERFLOW:
		signo = SIGFPE;
		break;
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_PRIV_INSTRUCTION:
		signo = SIGILL;
		break;
	case EXCEPTION_NONCONTINUABLE_EXCEPTION:
	case EXCEPTION_INVALID_DISPOSITION:
		{
			DECLARE_STR(1);

			print_str("Got fatal exception -- crashing.\n");
			flush_err_str();
			if (log_stdout_is_distinct())
				flush_str(STDOUT_FILENO);
		}
		break;
	default:
		{
			char buf[ULONG_DEC_BUFLEN];
			const char *s;
			DECLARE_STR(3);

			s = print_number(buf, sizeof buf, er->ExceptionCode);
			print_str("Got unknown exception #");		/* 0 */
			print_str(s);								/* 1 */
			print_str(" -- crashing.\n");				/* 2 */
			flush_err_str();
			if (log_stdout_is_distinct())
				flush_str(STDOUT_FILENO);
		}
		break;
	}

	/*
	 * Core dumps are not a standard Windows feature, so when we terminate
	 * it will be too late to collect information.  Attempt to trace the
	 * stack the process was in at the time of the exception.
	 *
	 * The mingw_stack[] array is in the BSS, not on the stack to minimize
	 * the runtime requirement in the exception routine.
	 *
	 * Because the current stack is apparently a dedicated exception stack,
	 * we have to get a stacktrace from the saved stack context at the
	 * time the exception occurred.  When calling mingw_sigraise(), the
	 * default crash handler will print the exception stack (the current one)
	 * which will prove rather useless.
	 *
	 * We only attempt to unwind the stack when we're hitting the first
	 * exception: recursive calls are not interesting.
	 */

	if (1 == in_exception_handler) {
		int count;
		
		count = mingw_stack_unwind(
			mingw_stack, G_N_ELEMENTS(mingw_stack), ei->ContextRecord, 0);

		stacktrace_stack_safe_print(STDERR_FILENO, mingw_stack, count);
		if (log_stdout_is_distinct())
			stacktrace_stack_safe_print(STDOUT_FILENO, mingw_stack, count);

		crash_save_stackframe(mingw_stack, count);
	} else if (in_exception_handler > 5) {
		DECLARE_STR(1);

		print_str("Too many exceptions in a row -- raising SIGBART.\n");
		flush_err_str();
		if (log_stdout_is_distinct())
			flush_str(STDOUT_FILENO);
		signo = SIGABRT;
	}

	/*
	 * Synthesize signal, as the UNIX kernel would for these exceptions.
	 */

	if (signo != 0)
		mingw_sigraise(signo);

	return EXCEPTION_CONTINUE_SEARCH;
}

static inline void WINAPI
mingw_invalid_parameter(const wchar_t * expression,
	const wchar_t * function, const wchar_t * file, unsigned int line,
   uintptr_t pReserved) 
{
	(void) expression;
	(void) function;
	(void) pReserved;
	
	wprintf(L"mingw: Invalid parameter in %s %s:%d\r\n", function, file, line);
}

#ifdef EMULATE_SBRK
static void *current_break;

/**
 * @return the initial break value, as defined by the first memory address
 * where HeapAlloc() allocates memory from.
 */
static void *
mingw_get_break(void)
{
	void *p;

	p = HeapAlloc(GetProcessHeap(), HEAP_NO_SERIALIZE, 1);

	if (NULL == p) {
		errno = ENOMEM;
		return (void *) -1;
	}

	HeapFree(GetProcessHeap(), HEAP_NO_SERIALIZE, p);
	return p;
}

/**
 * Add/remove specified amount of new core.
 *
 * The aim here is not to be able to do a malloc() but rather to mimic
 * what can be achieved on UNIX systems with sbrk().
 *
 * @return the prior break position.
 */
void *
mingw_sbrk(long incr)
{
	void *p;
	void *end;

	if (0 == incr) {
		p = mingw_get_break();
		if G_UNLIKELY(NULL == current_break)
			current_break = p;
		return p;
	} else if (incr > 0) {
		p = HeapAlloc(GetProcessHeap(), HEAP_NO_SERIALIZE, incr);

		if (NULL == p) {
			errno = ENOMEM;
			return (void *) -1;
		}

		end = ptr_add_offset(p, incr);

		if G_UNLIKELY(NULL == current_break)
			current_break = p;

		if (ptr_cmp(end, current_break) > 0)
			current_break = end;

		return p;
	} else if (incr < 0) {

		/*
		 * Don't release memory.  We have no idea how HeapAlloc() and
		 * HeapFree() work, and if they are like malloc(), then HeapFree()
		 * will frown upon a request for releasing core coming from coalesced
		 * blocks.
		 *
		 * That's OK, since sbrk() is only used in gtk-gnutella by xmalloc()
		 * to be able to allocate memory at startup time until the VMM layer
		 * is up.  The unfreed memory won't be lost.
		 *
		 * On Windows, the C runtime should not depend on malloc() however,
		 * so very little memory, if any, should be allocated on the heap
		 * before the VMM layer can be brought up.
		 */

		/* No memory was released, but fake a successful break decrease */
		return ptr_add_offset(current_break, -incr);
	}

	g_assert_not_reached();
}
#endif 	/* EMULATE_SBRK */

#ifdef MINGW_STARTUP_DEBUG
static FILE *
getlog(bool initial)
{
	return fopen("gtkg-log.txt", initial ? "wb" : "ab");
}

#define STARTUP_DEBUG(...)	G_STMT_START {	\
	if (lf != NULL) {						\
		fprintf(lf, __VA_ARGS__);			\
		fputc('\n', lf);					\
		fflush(lf);							\
	}										\
} G_STMT_END

#else	/* !MINGW_STARTUP_DEBUG */
#define getlog(x)	NULL
#define STARTUP_DEBUG(...)
#endif	/* MINGW_STARTUP_DEBUG */

static char mingw_stdout_buf[1024];		/* Used as stdout buffer */

static G_GNUC_COLD void
mingw_stdio_reset(FILE *lf, bool console)
{
	(void) lf;			/* In case no MINGW_STARTUP_DEBUG */

	/*
	 * A note on setvbuf():
	 *
	 * Setting _IONBF on Windows for output is a really bad idea because
	 * this results in a write for every character emitted.
	 *
	 * Setting _IOLBF on output for "binary" I/O is not working as expected
	 * because of the lack of "\r\n" termination.  It will require explicit
	 * fflush() calls in the logging layer.
	 */

	if (console) {
		int tty;
		
		tty = isatty(STDIN_FILENO);
		STARTUP_DEBUG("stdin is%s a tty", tty ? "" : "n't");
		if (tty) {
			fclose(stdin);
			close(STDIN_FILENO);
			freopen("CONIN$", "rb", stdin);
		} else {
			setmode(fileno(stdin), O_BINARY);
			STARTUP_DEBUG("forced stdin (fd=%d) to binary mode",
				fileno(stdout));
		}
		setvbuf(stdin, NULL, _IONBF, 0);	/* stdin must be unbuffered */
		tty = isatty(STDOUT_FILENO);
		STARTUP_DEBUG("stdout is%s a tty", tty ? "" : "n't");
		if (tty) {
			fclose(stdout);
			close(STDOUT_FILENO);
			freopen("CONOUT$", "w", stdout);	/* Not "wb" */
			/* stdout to a terminal is line-buffered */
			setvbuf(stdout, mingw_stdout_buf, _IOLBF, sizeof mingw_stdout_buf);
			STARTUP_DEBUG("forced stdout (fd=%d) to buffered "
				"(%lu bytes) binary mode",
				fileno(stdout), (ulong) sizeof mingw_stdout_buf);
		} else {
			setmode(fileno(stdout), O_BINARY);
			STARTUP_DEBUG("forced stdout (fd=%d) to binary mode",
				fileno(stdout));
		}
		tty = isatty(STDERR_FILENO);
		STARTUP_DEBUG("stderr is%s a tty", tty ? "" : "n't");
		if (tty) {
			fclose(stderr);
			close(STDERR_FILENO);
			freopen("CONOUT$", "w", stderr);	/* Not "wb" */
			setvbuf(stderr, NULL, _IOLBF, 0);
		} else {
			setmode(fileno(stderr), O_BINARY);
			STARTUP_DEBUG("forced stderr (fd=%d) to binary mode",
				fileno(stderr));
		}
	} else {
		fclose(stdin);
		fclose(stdout);
		fclose(stderr);
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		STARTUP_DEBUG("stdio fully reset");
	}
}

G_GNUC_COLD void
mingw_early_init(void)
{
	int console_err;
	FILE *lf = getlog(TRUE);

	STARTUP_DEBUG("starting PID %d", getpid());
	STARTUP_DEBUG("logging on fd=%d", fileno(lf));

#if __MSVCRT_VERSION__ >= 0x800
	STARTUP_DEBUG("configured invalid parameter handler");
	_set_invalid_parameter_handler(mingw_invalid_parameter);
#endif

	/* Disable any Windows pop-up on crash or file access error */
	SetErrorMode(SEM_NOOPENFILEERRORBOX | SEM_FAILCRITICALERRORS |
		SEM_NOGPFAULTERRORBOX);
	STARTUP_DEBUG("disabled Windows crash pop-up");

	/* Trap all unhandled exceptions */
	SetUnhandledExceptionFilter(mingw_exception);
	STARTUP_DEBUG("configured exception handler");

	_fcloseall();
	lf = getlog(FALSE);
	if (NULL == lf) {
		lf = getlog(TRUE);
		STARTUP_DEBUG("had to recreate this logfile for PID %d", getpid());
	} else {
		STARTUP_DEBUG("reopening of this logfile successful");
	}

	STARTUP_DEBUG("attempting AttachConsole()...");

	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		STARTUP_DEBUG("AttachConsole() succeeded");
		mingw_stdio_reset(lf, TRUE);
	} else {
		console_err = GetLastError();

		STARTUP_DEBUG("AttachConsole() failed, error = %d", console_err);

		switch (console_err) {
		case ERROR_INVALID_HANDLE:
		case ERROR_GEN_FAILURE:
			/* We had no console, and we got no console. */
			mingw_stdio_reset(lf, FALSE);
			freopen("NUL", "rb", stdin);
			STARTUP_DEBUG("stdin reopened from NUL");
			{
				const char *pathname;

				pathname = mingw_getstdout_path();
				STARTUP_DEBUG("stdout file will be %s", pathname);
				if (NULL != freopen(pathname, "wb", stdout)) {
					log_set(LOG_STDOUT, pathname);
					STARTUP_DEBUG("stdout (unbuffered) reopened");
				} else {
					STARTUP_DEBUG("could not reopen stdout");
				}

				pathname = mingw_getstderr_path();
				STARTUP_DEBUG("stderr file will be %s", pathname);
				if (NULL != freopen(pathname, "wb", stderr)) {
					log_set(LOG_STDERR, pathname);
					STARTUP_DEBUG("stderr (unbuffered) reopened");
				} else {
					STARTUP_DEBUG("could not reopen stderr");
				}
			}
			break;
		case ERROR_ACCESS_DENIED:
			/* Ignore, we already have a console */
			STARTUP_DEBUG("AttachConsole() denied");
			break;
		default:
			STARTUP_DEBUG("AttachConsole() has unhandled error");
			break;
		}
	}

	if (lf != NULL)
		fclose(lf);

	set_folder_basepath_func(mingw_get_folder_basepath);
}

void 
mingw_close(void)
{
	mingw_adns_close();
	
	if (libws2_32 != NULL) {
		FreeLibrary(libws2_32);
		
		libws2_32 = NULL;
		WSAPoll = NULL;
	}
}

#endif	/* MINGW32 */

/* vi: set ts=4 sw=4 cindent: */
