/*
 * Copyright (c) 2006, Christian Biere
 * Copyright (c) 2006, 2011, Raphael Manfredi
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
 * Virtual Memory Management (VMM).
 *
 * This is the lowest-level memory allocator, dealing with memory regions
 * at the granularity of a memory page (usually 4 KiB).
 *
 * Although the application can use this layer directly, it should rely on
 * other memory allocators such as walloc(), a wrapping layer over zalloc(),
 * or halloc() when tracking the size of the allocated area is impractical
 * or just impossible.  These allocators are in turn built on top of VMM.
 *
 * The VMM layer maintains a map of the virtual address space in order to
 * reduce memory fragmentation: we're making every attempt to avoid creating
 * fragments, which would be harmful in a 32-bit virtual address space as it
 * would end-up preventing the creation of large chunks of virtual memory.
 *
 * At the same time, in order to avoid exercising the kernel virtual memory
 * management code too often, we're maintaining a cache of small pages,
 * possibly coalescing them into bigger areas before releasing them or recycling
 * them because the process needs to allocate more memory.  Coalesced areas
 * can be split to fulfill smaller allocations, if necessary.
 *
 * Because we're not the kernel, we cannot get an accurate vision on the
 * usage of the virtual memory space.  Sometimes allocation at a given place
 * will fail, because for instance the kernel mapped a shared library there.
 * Such spots are marked as "foreign" memory zones, i.e. areas of the memory
 * that we did not allocate.
 *
 * The usage of UNIX mmap() and munmap() system calls should be avoided in
 * the application, preferring the wrappers vmm_mmap() and vmm_munmap() because
 * this lets us "see" the memory-mapped zones as "foreign" zones, without
 * having to discover them: since these zones are transient, most of the time,
 * it would cause a permanent waste of the memory virtual space: a region is
 * marked "foreign" once and is never forgotten, unless it was created via
 * vmm_mmap() and is now released through vmm_munmap().
 *
 * Virtual memory is allocated through vmm_alloc() or vmm_alloc0() and is
 * released through vmm_free().  The application should always strive to
 * give back memory through vmm_free() as soon as it no longer needs it.
 * It must carefully give back as much as was allocated though, so it must
 * keep track of the size of the allocated zone.  Because we're tracking
 * the allocated areas in memory, assertions can detect blatant misuses, but
 * cannot trap all the errors.
 *
 * When compiled with TRACK_VMM, a record of allocated virtual memory is kept
 * to allow leak detection (block of page allocated and never freed) or bad
 * freeing attempts (trying to free an address that was never allocated).
 * The tracking code also supports general MALLOC_FRAMES and MALLOC_TIME
 * compile options, to record allocation frames for an address and the time
 * at which the allocation took place.
 *
 * @attention
 * The initialization of the various memory allocators in the application
 * is tricky.  Although vmm_init() will necessarily be the first call made,
 * other allocators may need to be initialized, and the initialization order
 * is important.
 *
 * We distinguish two different type of memory regions here: "user" and "core".
 *
 * A "user" region is one allocated explicitly by user code to use as a
 * storage area. It is allocated by vmm_alloc() and needs to be released by
 * calling vmm_free().
 *
 * A "core" region is one allocated by other memory allocators and which will
 * be broken up into pieces possibly before being distributed to users.
 *
 * Portions of "core" regions may be freed at any time, whereas "user" regions
 * are allocated and freed atomically.  No "user" region should leak, but "core"
 * regions can and will leak due to possible memory fragmentation at the memory
 * allocator level.
 *
 * @author Christian Biere
 * @date 2006
 * @author Raphael Manfredi
 * @date 2006, 2009, 2011
 */

#include "common.h"

#define VMM_SOURCE
#include "vmm.h"

#include "ascii.h"
#include "cq.h"
#include "crash.h"			/* For crash_hook_add() */
#include "dump_options.h"
#include "fd.h"
#include "glib-missing.h"
#include "log.h"
#include "memusage.h"
#include "mutex.h"
#include "omalloc.h"
#include "parse.h"
#include "pow2.h"
#include "spinlock.h"
#include "str.h"			/* For str_bprintf() */
#include "stringify.h"
#include "tm.h"
#include "unsigned.h"
#include "xmalloc.h"

#ifdef TRACK_VMM
#include "hashtable.h"
#include "stacktrace.h"
#endif

#ifdef MINGW32
#include <windows.h>
#endif

#include "override.h"		/* Must be the last header included */

/*
 * With VMM_INVALIDATE_FREE_PAGES freed pages are invalidated so that the
 * system can recycle them without ever paging them out as we don't care about
 * the data in them anymore.
 *
 * This may cause the kernel to re-initialize a page with zeroes when the
 * page is un-cached, and causes an extra system call each time the page is
 * put in the cache or removed from it, which is a non negligeable penalty,
 * not even counting the work the kernel has to do.  This partly defeats the
 * advantage of having a cache to quickly recycle pages.
 *
 * Therefore, by default it is not enabled.
 */
#if 0
#define VMM_INVALIDATE_FREE_PAGES
#endif

/*
 * With VMM_PROTECT_FREE_PAGES freed pages are completely protected. This may
 * help to detect access-after-free bugs.
 */
#if 0
#define VMM_PROTECT_FREE_PAGES
#endif

static size_t kernel_pagesize = 0;
static size_t kernel_pagemask = 0;
static unsigned kernel_pageshift = 0;
static gboolean kernel_mapaddr_increasing;

#define VMM_CACHE_SIZE		256	/**< Amount of entries per cache line */
#define VMM_CACHE_LINES		32	/**< Amount of cache lines */
#define VMM_CACHE_LIFE		60	/**< At most 1 minute if not fragmenting */
#define VMM_CACHE_MAXLIFE	180	/**< At most 3 minutes if fragmenting */
#define VMM_STACK_MINSIZE	(64 * 1024)		/**< Minimum stack size */
#define VMM_FOREIGN_LIFE	(60 * 60)		/**< 60 minutes */
#define VMM_FOREIGN_MAXLEN	(512 * 1024)	/**< 512 KiB */

struct page_info {
	void *base;		/**< base address */
	time_t stamp;	/**< time at which the page was inserted */
};

struct page_cache {
	struct page_info info[VMM_CACHE_SIZE];	/**< sorted on base address */
	spinlock_t lock;	/**< thread-safe protection */
	size_t current;		/**< amount of items in info[] */
	size_t pages;		/**< amount of consecutive pages for entries */
	size_t chunksize;	/**< size of each entry */
};

/**
 * The page cache.
 *
 * At index 0, we have pages whose size is the VMM page size.
 * Ad index n, we have areas made of n+1 consecutive pages.
 */
static struct page_cache page_cache[VMM_CACHE_LINES];

/**
 * Fragment types
 */
typedef enum {
	VMF_NATIVE	= 0,		/**< Allocated by this layer */
	VMF_MAPPED	= 1,		/**< Memory-mapped by this layer */
	VMF_FOREIGN	= 2			/**< Foreign region */
} vmf_type_t;

/**
 * Structure used to represent a fragment in the virtual memory space.
 */
struct vm_fragment {
	const void *start;		/**< Start address */
	const void *end;		/**< First byte beyond end of region */
	time_t mtime;			/**< Last time we updated fragment */
	vmf_type_t type;		/**< Fragment type */
};

/**
 * A process virtual memory map.
 *
 * The array holding the sorted fragments is allocated via alloc_pages().
 */
struct pmap {
	struct vm_fragment *array;		/**< Sorted array of vm_fragment structs */
	mutex_t lock;					/**< Thread-safe locking */
	size_t count;					/**< Amount of entries in array */
	size_t size;					/**< Total amount of slots in array */
	size_t pages;					/**< Amount of pages for the array */
	size_t generation;				/**< Reloading generation number */
	unsigned loading:1;				/**< Pmap being loaded */
	unsigned resized:1;				/**< Pmap has been resized */
	unsigned extending:1;			/**< Pmap being extended */
};

/**
 * Internal statistics collected.
 */
static struct {
	guint64 allocations;			/**< Total number of allocations */
	guint64 allocations_zeroed;		/**< Total number of zeroing in allocs */
	guint64 freeings;				/**< Total number of freeings */
	guint64 shrinkings;				/**< Total number of shrinks */
	guint64 mmaps;					/**< Total number of mmap() calls */
	guint64 munmaps;				/**< Total number of munmap() calls */
	guint64 hints_followed;			/**< Allocations following hints */
	guint64 hints_ignored;			/**< Allocations ignoring non-NULL hints */
	guint64 alloc_from_cache;		/**< Allocation from cache */
	guint64 alloc_from_cache_pages;	/**< Pages allocated from cache */
	guint64 alloc_direct_core;		/**< Allocation done through core */
	guint64 alloc_direct_core_pages;/**< Pages allocated from core */
	guint64 free_to_cache;			/**< Freeings directed to cache */
	guint64 free_to_cache_pages;	/**< Amount of pages directed to cache */
	guint64 free_to_system;			/**< Freeings returned to kernel */
	guint64 free_to_system_pages;	/**< Amount of pages returned to kernel */
	guint64 forced_freed;			/**< Amount of forceful freeings */
	guint64 forced_freed_pages;		/**< Amount of pages forcefully freed */
	guint64 cache_evictions;		/**< Evictions due to cache being full */
	guint64 cache_coalescing;		/**< Amount of coalescing from cache */
	guint64 cache_line_coalescing;	/**< Amount of coalescing in cache line */
	guint64 cache_expired;			/**< Amount of entries expired from cache */
	guint64 cache_expired_pages;	/**< Expired pages from cache */
	guint64 high_order_coalescing;	/**< Large regions successfully coalesced */
	guint64 pmap_foreign_discards;	/**< Foreign regions discarded */
	guint64 pmap_foreign_discarded_pages;	/**< Foreign pages discarded */
	guint64 pmap_overruled;			/**< Regions overruled by kernel */
	size_t user_memory;				/**< Amount of "user" memory allocated */
	size_t user_pages;				/**< Amount of "user" memory pages used */
	size_t user_blocks;				/**< Amount of "user" memory blocks */
	size_t core_memory;				/**< Amount of "core" memory allocated */
	size_t core_pages;				/**< Amount of "core" memory pages used */
	/* Tracking core blocks doesn't make sense: "core" can be fragmented */
	memusage_t *user_mem;			/**< User memory usage statistics */
	memusage_t *core_mem;			/**< Core usage statistics */
} vmm_stats;

/**
 * The kernel version of the pmap and our own version.
 */
static struct pmap kernel_pmap;
static struct pmap local_pmap;

static gboolean safe_to_log;		/**< True when we can log */
static gboolean stop_freeing;		/**< No longer release memory */
static guint32 vmm_debug;			/**< Debug level */
static const void *initial_sp;		/**< Initial "bottom" of the stack */
static gboolean sp_increasing;		/**< Growing direction of the stack */
static const void *vmm_base;		/**< Where we'll start allocating */
#ifdef HAS_SBRK
static const void *initial_brk;		/**< Startup position of the heap */
#endif

#ifdef TRACK_VMM
static void vmm_track_init(void);
static void vmm_track_malloc_inited(void);
static void vmm_track_post_init(void);
static void vmm_track_close(void);
#endif

#define vmm_debugging(lvl)	G_UNLIKELY(vmm_debug > (lvl) && safe_to_log)

gboolean vmm_is_debugging(guint32 level) 
{ 
	return vmm_debugging(level);
}

static inline struct pmap *
vmm_pmap(void)
{
	return kernel_pmap.count ? &kernel_pmap : &local_pmap;
}

static gboolean page_cache_insert_pages(void *base, size_t n);
static void pmap_remove(struct pmap *pm, const void *p, size_t size);
static void pmap_load(struct pmap *pm);
static void pmap_insert_region(struct pmap *pm,
	const void *start, size_t size, vmf_type_t type);
static void pmap_overrule(struct pmap *pm,
	const void *p, size_t size, vmf_type_t type);
static void vmm_reserve_stack(size_t amount);

/**
 * Initialize constants for the computation of kernel page roundings.
 */
static void
init_kernel_pagesize(void)
{
	kernel_pagesize = compat_pagesize();
	g_assert(is_pow2(kernel_pagesize));
	kernel_pagemask = kernel_pagesize - 1;
	kernel_pageshift = ffs(kernel_pagesize) - 1;
}

/**
 * Fast version of pagesize rounding (without the slow % operator).
 */
static inline ALWAYS_INLINE size_t G_GNUC_PURE
round_pagesize_fast(size_t n)
{
	return (n + kernel_pagemask) & ~kernel_pagemask;
}

/**
 * Fast version of page counting: how many pages does it take to store `n'?
 */
static inline ALWAYS_INLINE size_t G_GNUC_PURE
pagecount_fast(size_t n)
{
	return round_pagesize_fast(n) >> kernel_pageshift;
}

/**
 * Rounds `n' up to so that it is aligned to the pagesize.
 */
size_t
round_pagesize(size_t n)
{
	return round_pagesize_fast(n);
}

/**
 * Rounds pointer down so that it is aligned to the start of the page.
 */
static inline G_GNUC_PURE ALWAYS_INLINE const void *
page_start(const void *p)
{
	unsigned long addr = pointer_to_ulong(p);

	addr &= ~kernel_pagemask;
	return ulong_to_pointer(addr);
}

/**
 * Rounds pointer down so that it is aligned to the start of its page.
 */
const void *
vmm_page_start(const void *p)
{
	return page_start(p);
}

/**
 * Count amount of pages required to hold given size.
 */
size_t
vmm_page_count(size_t size)
{
	return pagecount_fast(size);
}

static long
compat_pagesize_intern(void)
#if defined (_SC_PAGESIZE) || defined(_SC_PAGE_SIZE)
{
	long ret;

	errno = 0;
#if defined (_SC_PAGESIZE)
	ret = sysconf(_SC_PAGESIZE);
#else
	ret = sysconf(_SC_PAGE_SIZE);
#endif
	if (-1L == ret) {
		return_value_unless(0 == errno, 0);
	}
	return ret;
}
#else /* !_SC_PAGESIZE && !_SC_PAGE_SIZE */
{
	return getpagesize();
}
#endif /* _SC_PAGESIZE || _SC_PAGE_SIZE */

size_t
compat_pagesize(void)
{
	static int initialized;
	static size_t psize;

	if G_UNLIKELY(!initialized) {
		long n;
		
		initialized = 1;
		n = compat_pagesize_intern();
		g_assert(n > 0);
		g_assert(n < INT_MAX);
		g_assert(is_pow2(n));
		psize = n;
		g_assert((size_t) psize == (size_t) n);
	}

	return psize;
}

/**
 * @return whether fragment is identified as foreign.
 */
static inline ALWAYS_INLINE gboolean
vmf_is_foreign(const struct vm_fragment *vmf)
{
	return VMF_FOREIGN == vmf->type;
}

/**
 * @return whether fragment is identified as being memory-mapped.
 */
static inline ALWAYS_INLINE gboolean
vmf_is_mapped(const struct vm_fragment *vmf)
{
	return VMF_MAPPED == vmf->type;
}

/**
 * @return whether fragment is identified as being native.
 */
static inline ALWAYS_INLINE gboolean
vmf_is_native(const struct vm_fragment *vmf)
{
	return VMF_NATIVE == vmf->type;
}

/**
 * @return size of a memory fragment.
 */
static inline ALWAYS_INLINE size_t
vmf_size(const struct vm_fragment *vmf)
{
	return ptr_diff(vmf->end, vmf->start);
}

/**
 * @return whether memory fragment is an "old" foreign region thatwe may
 * discard.
 */
static inline ALWAYS_INLINE gboolean
vmf_is_old_foreign(const struct vm_fragment *vmf)
{
	/*
	 * We only discard "foreign" regions that are not too large, because we
	 * don't want to spend too much time attempting to use addresses from
	 * that range as allocation hints, in case it is a truly "foreign" region.
	 */

	return vmf_is_foreign(vmf) &&
		vmf_size(vmf) <= VMM_FOREIGN_MAXLEN &&
		delta_time(tm_time(), vmf->mtime) > VMM_FOREIGN_LIFE;
}

/**
 * @return textual description of memory fragment type.
 */
static const char *
vmf_type_str(const vmf_type_t type)
{
	switch (type) {
	case VMF_NATIVE:	return "native";
	case VMF_MAPPED:	return "mapped";
	case VMF_FOREIGN:	return "foreign";
	}

	g_error("corrupted VM fragment type: %d", type);
	return NULL;
}

/**
 * @return textual description of a memory fragment type in static buffer.
 */
static const char *
vmf_to_string(const struct vm_fragment *vmf)
{
	static char buf[80];
	size_t n = pagecount_fast(vmf_size(vmf));

	str_bprintf(buf, sizeof buf, "%s [%p, %p[ (%zu page%s)",
		vmf_type_str(vmf->type), vmf->start, vmf->end,
		n, 1 == n ? "" : "s");

	return buf;
}

/**
 * Dump current pmap to specified logagent.
 */
G_GNUC_COLD void
vmm_dump_pmap_log(logagent_t *la)
{
	struct pmap *pm = vmm_pmap();
	size_t i;
	time_t now = tm_time();

	mutex_get(&pm->lock);

	log_debug(la, "VMM current %s pmap (%zu region%s):",
		pm == &kernel_pmap ? "kernel" :
		pm == &local_pmap ? "local" : "unknown",
		pm->count, 1 == pm->count ? "" : "s");

	for (i = 0; i < pm->count; i++) {
		struct vm_fragment *vmf = &pm->array[i];
		size_t hole = 0;

		if (i < pm->count - 1) {
			const void *next = pm->array[i+1].start;
			hole = ptr_diff(next, vmf->end);
		}

		log_debug(la, "VMM [%p, %p] %zuKiB %s%s%s%s (%s)%s",
			vmf->start, const_ptr_add_offset(vmf->end, -1),
			vmf_size(vmf) / 1024, vmf_type_str(vmf->type),
			hole ? " + " : "",
			hole ? size_t_to_string(hole / 1024) : "",
			hole ? "KiB hole" : "",
			compact_time(delta_time(now, vmf->mtime)),
			size_is_non_negative(hole) ? "" : " *UNSORTED*");
	}

	mutex_release(&pm->lock);
}

/**
 * Dump current pmap.
 */
G_GNUC_COLD void
vmm_dump_pmap(void)
{
	vmm_dump_pmap_log(log_agent_stderr_get());
}

#if defined(HAS_MMAP) || defined(MINGW32)
/**
 * Find a hole in the virtual memory map where we could allocate "size" bytes.
 */
static const void *
vmm_find_hole(size_t size)
{
	struct pmap *pm = vmm_pmap();
	size_t i;

	mutex_get(&pm->lock);

	if G_UNLIKELY(0 == pm->count || pm->loading) {
		mutex_release(&pm->lock);
		return NULL;
	}

	if (kernel_mapaddr_increasing) {
		for (i = 0; i < pm->count; i++) {
			struct vm_fragment *vmf = &pm->array[i];
			const void *end = vmf->end;

			if G_UNLIKELY(ptr_cmp(end, vmm_base) < 0)
				continue;

			if G_UNLIKELY(i == pm->count - 1) {
				mutex_release(&pm->lock);
				return end;
			} else {
				struct vm_fragment *next = &pm->array[i + 1];

				if (ptr_diff(next->start, end) >= size) {
					mutex_release(&pm->lock);
					return end;
				}
			}
		}
	} else {
		for (i = pm->count; i > 0; i--) {
			struct vm_fragment *vmf = &pm->array[i - 1];
			const void *start = vmf->start;

			if G_UNLIKELY(ptr_cmp(start, vmm_base) > 0)
				continue;

			if G_UNLIKELY(1 == i) {
				mutex_release(&pm->lock);
				return page_start(const_ptr_add_offset(start, -size));
			} else {
				struct vm_fragment *prev = &pm->array[i - 2];

				if (ptr_diff(start, prev->end) >= size) {
					mutex_release(&pm->lock);
					return page_start(const_ptr_add_offset(start, -size));
				}
			}
		}
	}

	if (vmm_debugging(0)) {
		s_warning("VMM no %zuKiB hole found in pmap", size / 1024);
	}

	mutex_release(&pm->lock);
	return NULL;
}

/**
 * Discard foreign region at specified index within the pmap.
 */
static void
pmap_discard_index(struct pmap *pm, size_t idx)
{
	g_assert(pm != NULL);
	g_assert(size_is_non_negative(idx) && idx < pm->count);

	mutex_get(&pm->lock);

	if (vmm_debugging(0)) {
		struct vm_fragment *vmf = &pm->array[idx];

		g_assert_log(vmf_is_foreign(vmf), "vmf={%s}", vmf_to_string(vmf));

		s_debug("VMM discarding %s region at %p (%zuKiB) updated %us ago",
			vmf_type_str(vmf->type), vmf->start,
			vmf_size(vmf) / 1024, (unsigned) delta_time(tm_time(), vmf->mtime));

		vmm_stats.pmap_foreign_discards++;
		vmm_stats.pmap_foreign_discarded_pages += pagecount_fast(vmf_size(vmf));
	}

	if G_LIKELY(idx != pm->count - 1) {
		memmove(&pm->array[idx], &pm->array[idx + 1],
			(pm->count - idx - 1) * sizeof pm->array[0]);
	}

	pm->count--;

	mutex_release(&pm->lock);
}

/**
 * Compute the size of the first hole at the base of the VM space and return
 * its location in ``hole_ptr'', if we find one with a non-zero length.
 *
 * @param hole_ptr		where first hole address is written to
 * @param discard		whether we can discard foreign regions during lookup
 *
 * @return size of the first hole we found, 0 if none found.
 *
 * @attention
 * When kernel_mapaddr_increasing is FALSE, the value in hole_ptr is the end
 * of the memory region.  To get the starting address of the hole�(i.e. the
 * memory pointer that would get allocated to cover the hole), one has to
 * substract the size of the region from the returned pointer.
 */
static G_GNUC_HOT size_t
vmm_first_hole(const void **hole_ptr, gboolean discard)
{
	struct pmap *pm = vmm_pmap();
	size_t i;

	mutex_get(&pm->lock);

	if G_UNLIKELY(0 == pm->count) {
		mutex_release(&pm->lock);
		return 0;
	}

	if (kernel_mapaddr_increasing) {
		for (i = 0; i < pm->count; i++) {
			struct vm_fragment *vmf = &pm->array[i];
			const void *end = vmf->end;
			size_t len;

			if G_UNLIKELY(ptr_cmp(end, vmm_base) < 0)
				continue;

			if G_UNLIKELY(i == pm->count - 1) {
				*hole_ptr = end;
				mutex_release(&pm->lock);
				return SIZE_MAX;
			} else {
				struct vm_fragment *next = &pm->array[i + 1];

				/*
				 * If we are at a foreign region that has not been updated
				 * for some time, discard it and try to reuse that region.
				 */

				if G_UNLIKELY(discard && vmf_is_old_foreign(vmf)) {
					const void *start = vmf->start;
					pmap_discard_index(pm, i);
					*hole_ptr = start;
					len = ptr_diff(next->start, start);
					mutex_release(&pm->lock);
					return len;
				}

				if (next->start == end)
					continue;		/* Different types do not coalesce */
				*hole_ptr = end;
				len = ptr_diff(next->start, end);
				mutex_release(&pm->lock);
				return len;
			}
		}
	} else {
		for (i = pm->count; i > 0; i--) {
			struct vm_fragment *vmf = &pm->array[i - 1];
			const void *start = vmf->start;
			size_t len;

			if G_UNLIKELY(ptr_cmp(start, vmm_base) > 0)
				continue;

			if G_UNLIKELY(1 == i) {
				*hole_ptr = ulong_to_pointer(kernel_pagesize);	/* Not NULL */
				mutex_release(&pm->lock);
				return SIZE_MAX;
			} else {
				struct vm_fragment *prev = &pm->array[i - 2];

				/*
				 * If we are at a foreign region that has not been updated
				 * for some time, discard it and try to reuse that region.
				 */

				if G_UNLIKELY(discard && vmf_is_old_foreign(vmf)) {
					const void *end = vmf->end;
					pmap_discard_index(pm, i - 1);
					*hole_ptr = end;
					len = ptr_diff(end, prev->end);
					mutex_release(&pm->lock);
					return len;
				}

				if (start == prev->end)
					continue;		/* Foreign and native do not coalesce */
				*hole_ptr = start;
				len = ptr_diff(start, prev->end);
				mutex_release(&pm->lock);
				return len;
			}
		}
	}

	mutex_release(&pm->lock);
	return 0;
}

/**
 * Wrapper on mmap() and munmap() to allocate/free anonymous memory.
 */
#ifdef MINGW32
static inline void *
vmm_valloc(void *hint, size_t size)
{
	return mingw_valloc(hint, size);
}

static inline int
vmm_vfree(void *addr, size_t size)
{
	return mingw_vfree(addr, size);
}

static inline int
vmm_vfree_fragment(void *addr, size_t size)
{
	return mingw_vfree_fragment(addr, size);
}
#else	/* !MINGW32 */
static void *
vmm_valloc(void *hint, size_t size)
{
	static int fd = -1;
	int flags;

#if defined(MAP_ANON)
	flags = MAP_PRIVATE | MAP_ANON;
#elif defined (MAP_ANONYMOUS)
	flags = MAP_PRIVATE | MAP_ANONYMOUS;
#else
	flags = MAP_PRIVATE;
	if (-1 == fd) {
		fd = get_non_stdio_fd(open("/dev/zero", O_RDWR, 0));
		return_value_unless(fd >= 0, NULL);
		set_close_on_exec(fd);
	}
#endif	/* MAP_ANON */

	return mmap(hint, size, PROT_READ | PROT_WRITE, flags, fd, 0);
}

static inline int
vmm_vfree(void *addr, size_t size)
{
	return munmap(addr, size);
}

static inline int
vmm_vfree_fragment(void *addr, size_t size)
{
	return munmap(addr, size);
}
#endif	/* MINGW32 */

#else	/* !HAS_MMAP && !MINGW32 */
static inline size_t
vmm_first_hole(const void **unused, gboolean discard)
{
	(void) unused;
	(void) discard;
	return 0;
}
#endif	/* HAS_MMAP || MINGW32 */

/**
 * Insert foreign region in the pmap.
 */
static void
pmap_insert_foreign(struct pmap *pm, const void *start, size_t size)
{
	pmap_insert_region(pm, start, size, VMF_FOREIGN);
}

#ifdef HAS_MMAP
/**
 * Insert memory-mapped region in the pmap.
 */
static void
pmap_insert_mapped(struct pmap *pm, const void *start, size_t size)
{
	pmap_insert_region(pm, start, size, VMF_MAPPED);
}
#endif	/* HAS_MMAP */

/**
 * Allocate a new chunk of anonymous memory.
 *
 * @param size		amount of bytes we want
 * @param hole		if non-NULL, identified VM hole of at least size bytes.
 */
static void *
vmm_mmap_anonymous(size_t size, const void *hole)
#if defined(HAS_MMAP) || defined(MINGW32)
{
	static int failed;
	void *hint, *p;
	static guint64 hint_followed;

	if G_UNLIKELY(failed)
		return NULL;

	if (G_UNLIKELY(stop_freeing)) {
		hint = NULL;
	} else {
		hint = deconstify_gpointer(NULL == hole ? vmm_find_hole(size) : hole);
	}

	if G_UNLIKELY(hint != NULL) {
		if (vmm_debugging(8)) {
			s_debug("VMM hinting %s%p for new %zuKiB region",
				NULL == hole ? "" : "supplied ", hint, size / 1024);
		}
	}

	p = vmm_valloc(hint, size);

	if G_UNLIKELY(MAP_FAILED == p) {
		if (ENOMEM != errno) {
			failed = 1;
			return_value_unless(MAP_FAILED != p, NULL);
		}
		p = NULL;
	} else if G_UNLIKELY(p != hint) {
		if G_UNLIKELY(hint != NULL) {
			if (vmm_debugging(0)) {
				s_warning("VMM kernel did not follow hint %p for %zuKiB "
					"region, picked %p (after %zu followed hint%s)",
					hint, size / 1024, p, (size_t) hint_followed,
					hint_followed == 1 ? "" : "s");
			}
			vmm_stats.hints_ignored++;
		}

		/*
		 * Because the hint was not followed and we're not dealing with
		 * "foreign" memory here, we have to over-rule any chunk of the map
		 * space that we could have declared as "foreign" and which would
		 * overlap the space we just mapped.
		 *
		 * This can happen when we declared "foreign" some pages because
		 * our past allocations at these hinted spots failed, but for some
		 * reason the memory was released and we could not notice it.
		 */

		pmap_overrule(vmm_pmap(), p, size, VMF_NATIVE);

		if (NULL == hint)
			goto done;

		/*
		 * Kernel did not use our hint, maybe it was wrong because something
		 * got mapped at the place where we thought there was nothing...
		 *
		 * Reload the current kernel pmap if we're able to do so, otherwise
		 * see whether we can mark something as "foreign" in the VM space so
		 * that we avoid further attempts at the same location for blocks
		 * of similar sizes.
		 */

		hint_followed = 0;

		if (vmm_pmap() == &kernel_pmap) {
			if (vmm_debugging(0)) {
				s_debug("VMM current kernel pmap before reloading attempt:");
				vmm_dump_pmap();
			}
			pmap_load(&kernel_pmap);
			vmm_reserve_stack(0);
		} else if (!local_pmap.extending) {
			if (size <= kernel_pagesize) {
				pmap_insert_foreign(&local_pmap, hint, kernel_pagesize);
				if (vmm_debugging(0)) {
					s_debug("VMM marked hint %p as foreign", hint);
				}
			} else if (
				ptr_cmp(hint, ptr_add_offset(p, size)) >= 0 ||
				ptr_cmp(hint, p) < 0
			) {
				void *try;

				/*
				 * The hint address is not included in the allocated segment.
				 *
				 * Try allocating a single page at the hint location, and
				 * if we don't succeed, we can mark it as foreign.  If
				 * we succeed and we were previously trying to allocate
				 * exactly two pages, then we know the next page is foreign.
				 */

				try = vmm_valloc(hint, kernel_pagesize);

				if G_LIKELY(try != MAP_FAILED) {
					if (try != hint) {
						pmap_insert_foreign(&local_pmap, hint, kernel_pagesize);
						if (vmm_debugging(0)) {
							s_debug("VMM marked hint %p as foreign", hint);
						}
					} else if (2 == pagecount_fast(size)) {
						void *next = ptr_add_offset(hint, kernel_pagesize);
						if (next != p) {
							pmap_insert_foreign(&local_pmap, next,
								kernel_pagesize);
							if (vmm_debugging(0)) {
								s_debug("VMM marked %p (page after %p) "
									"as foreign", next, hint);
							}
						} else {
							if (vmm_debugging(0)) {
								s_debug("VMM funny kernel ignored hint %p "
									"and allocated 8 KiB at %p whereas hint "
									"was free", hint, p);
							}
						}
					} else {
						if (vmm_debugging(1)) {
							s_debug("VMM hinted %p is not a foreign page",
								hint);
						}
					}
					if (0 != vmm_vfree(try, kernel_pagesize)) {
						s_warning("VMM cannot free single page at %p: %m", try);
					}

				} else {
					if (vmm_debugging(0)) {
						s_warning("VMM cannot allocate one page at %p: %m",
							hint);
					}
				}
			} else {
				if (vmm_debugging(0)) {
					s_debug("VMM hint %p fell within allocated [%p, %p]",
						hint, p, ptr_add_offset(p, size - 1));
				}
			}
		}
	} else if (hint != NULL) {
		if G_UNLIKELY(0 == (hint_followed & 0xff)) {
			if (vmm_debugging(0)) {
				s_debug("VMM hint %p followed for %zuKiB (%zu consecutive)",
					hint, size / 1024, (size_t) hint_followed);
			}
		}
		hint_followed++;
		vmm_stats.hints_followed++;
	}
done:
	return p;
}
#else	/* !HAS_MMAP */
{
	void *p;

	(void) hole;
	size = round_pagesize_fast(size);
#if defined(HAS_POSIX_MEMALIGN)
	if (posix_memalign(&p, kernel_pagesize, size)) {
		p = NULL;
	}
#elif defined(HAS_MEMALIGN)
	p = memalign(kernel_pagesize, size);
#else
	(void) size;
	p = NULL;
	g_assert_not_reached();
#error "Neither mmap(), posix_memalign() nor memalign() available"
#endif	/* HAS_POSIX_MEMALIGN */
	if (p) {
		memset(p, 0, size);
	}
	return p;
}
#endif	/* HAS_MMAP */

/**
 * Insert region in the pmap, known to be native.
 */
static void
pmap_insert(struct pmap *pm, const void *start, size_t size)
{
	pmap_insert_region(pm, start, size, VMF_NATIVE);
}

/**
 * Allocates a page-aligned chunk of memory.
 *
 * @param size			the amount of bytes to allocate.
 * @param update_pmap	whether our VMM pmap should be updated
 * @param hole			if non-NULL, identified VM hole of size bytes at least
 *
 * @return On success a pointer to the allocated chunk is returned. On
 *		   failure NULL is returned.
 */
static void *
alloc_pages(size_t size, gboolean update_pmap, const void *hole)
{
	void *p;
	size_t generation = kernel_pmap.generation;

	g_assert(kernel_pagesize > 0);

	p = vmm_mmap_anonymous(size, hole);

	return_value_unless(NULL != p, NULL);
	g_assert_log(page_start(p) == p, "aligned memory required: %p", p);

	if (vmm_debugging(5)) {
		s_debug("VMM allocated %zuKiB region at %p", size / 1024, p);
	}

	/*
	 * Since the kernel pmap can be reloaded by vmm_mmap_anonymous(), we
	 * need to be careful and not insert something that will be listed there.
	 *
	 * NB: we check the kernel_pmap object only but we use vmm_pmap() for
	 * inserting in case we do not use the kernel pmap.
	 */

	if (update_pmap && kernel_pmap.generation == generation)
		pmap_insert(vmm_pmap(), p, size);

	return p;
}

/**
 * Release pages allocated by alloc_pages().
 * Must not be called directly other than through free_pages*() routines.
 */
static void
free_pages_intern(void *p, size_t size, gboolean update_pmap)
{
	/*
	 * If ``stop_freeing'' was set, well be only updating the pmap so that
	 * we can spot "leaks" at vmm_close() time.
	 */

	if (G_UNLIKELY(stop_freeing))
		goto pmap_update;

#if defined(HAS_MMAP) || defined(MINGW32)
	{
		int ret = 0;

		ret = vmm_vfree_fragment(p, size);
		return_unless(0 == ret);
	}
#elif defined(HAS_POSIX_MEMALIGN) || defined(HAS_MEMALIGN)
	(void) size;
	free(p);
#else
	(void) p;
	(void) size;
	g_assert_not_reached();
#error "Neither mmap(), posix_memalign() nor memalign() available"
#endif	/* HAS_POSIX_MEMALIGN || HAS_MEMALIGN */

pmap_update:
	if (update_pmap)
		pmap_remove(vmm_pmap(), p, size);
}

/**
 * Free memory allocated by alloc_pages().
 */
static void
free_pages(void *p, size_t size, gboolean update_pmap)
{
	if (vmm_debugging(5)) {
		s_debug("VMM freeing %zuKiB region at %p", size / 1024, p);
	}

	free_pages_intern(p, size, update_pmap);
}

/**
 * Lookup address within the pmap.
 *
 * If ``low_ptr'' is non-NULL, it is written with the index where insertion
 * of a new item should happen (in which case the returned value must be NULL)
 * or with the index of the found fragment within the pmap.
 *
 * @return found fragment if address lies within the fragment, NULL if
 * no fragment containing the address was found.
 */
static G_GNUC_HOT struct vm_fragment *
pmap_lookup(const struct pmap *pm, const void *p, size_t *low_ptr)
{
	size_t low = 0, high = pm->count - 1;
	struct vm_fragment *item;
	size_t mid = 0;

	/* Binary search */

	for (;;) {
		if (low > high || high > SIZE_MAX / 2) {
			item = NULL;		/* Not found */
			break;
		}

		mid = low + (high - low) / 2;

		g_assert(mid < pm->count);

		item = &pm->array[mid];
		if (ptr_cmp(p, item->end) >= 0)
			low = mid + 1;
		else if (ptr_cmp(p, item->start) < 0)
			high = mid - 1;
		else
			break;	/* Found */
	}

	if (low_ptr != NULL)
		*low_ptr = (item == NULL) ? low : mid;

	return item;
}

/**
 * Allocate a new pmap.
 */
static void
pmap_allocate(struct pmap *pm)
{
	g_assert(NULL == pm->array);
	g_assert(0 == pm->pages);

	mutex_init(&pm->lock);
	pm->array = alloc_pages(kernel_pagesize, FALSE, NULL);
	pm->pages = 1;
	pm->count = 0;
	pm->size = kernel_pagesize / sizeof pm->array[0];

	if (NULL == pm->array)
		s_error("cannot initialize the VMM layer: out of memory already?");

	pmap_insert(vmm_pmap(), pm->array, kernel_pagesize);
}

/**
 * Extend the pmap by allocating one more page.
 */
static void
pmap_extend(struct pmap *pm)
{
	struct vm_fragment *narray;
	size_t nsize;
	size_t osize;
	void *oarray;
	size_t old_generation;
	gboolean was_extending = pm->extending;

	g_assert(mutex_is_owned(&pm->lock));

retry:

	osize = kernel_pagesize * pm->pages;
	nsize = osize + kernel_pagesize;
	old_generation = vmm_pmap()->generation;

	if (vmm_debugging(0)) {
		s_debug("VMM extending %s%s%s pmap from %zu KiB to %zu KiB",
			pm->extending ? "(recursively) " : "",
			pm->loading ? "loading " : "",
			pm == &kernel_pmap ? "kernel" : "local",
			osize / 1024, nsize / 1024);
	}

	pm->extending = TRUE;

	/*
	 * It is possible to recursively enter here through alloc_pages() when
	 * mmap() is used to allocate virtual memory, in case we reload the
	 * kernel map or insert a foreign region.
	 *
	 * To protect against that, we start by allocating the new pages and
	 * remember the amount of pages in the map.  If upon return from the
	 * alloc_pages() call we see the number is different, it means the
	 * allocation was done and we can free up the pages we already allocated
	 * and return.
	 */

	{
		size_t old_pages = pm->pages;

		narray = alloc_pages(nsize, FALSE, NULL);	/* May recurse here */

		if (pm->pages != old_pages) {
			if (vmm_debugging(0)) {
				s_warning("VMM already recursed to pmap_extend(), "
					"pmap is now %zu KiB",
					(kernel_pagesize * pm->pages) / 1024);
			}
			g_assert(kernel_pagesize * pm->pages >= nsize);
			if (narray != NULL)
				free_pages(narray, nsize, FALSE);

			/*
			 * If after recursion we're left with a full pmap. we need to
			 * retry another extension.
			 */

			if (pm->count != pm->size)
				return;

			if (vmm_debugging(0))
				s_warning("VMM however pmap is still full, extending again...");
			
			goto retry;
		} else {
			if (vmm_debugging(0)) {
				s_debug("VMM allocated new %zu KiB %s pmap at %p",
					nsize / 1024, pm == &kernel_pmap ? "kernel" : "local",
					narray);
			}
		}

		if (NULL == narray)
			s_error("cannot extend pmap: out of virtual memory");
	}

	oarray = pm->array;
	pm->pages++;
	pm->size = nsize / sizeof pm->array[0];
	memcpy(narray, oarray, osize);
	pm->array = narray;
	if (!was_extending) {
		pm->extending = FALSE;
	}

	/*
	 * Freeing could update the pmap we've been extending.
	 *
	 * The structure must therefore be in a consistent state before the old
	 * array can be freed, so this must come last.
	 */

	free_pages(oarray, osize, TRUE);

	/*
	 * Watch out for extending the kernel pmap whilst we're reloading it.
	 * This means our data structures were too small, and we'll need to
	 * reload it again.
	 */

	{
		struct pmap *vpm = vmm_pmap();

		if (!vpm->loading) {
			if (vpm->generation == old_generation) {
				pmap_insert(vpm, narray, nsize);
			} else {
				if (vmm_debugging(0)) {
					s_debug("VMM kernel pmap reloaded during extension");
				}
				/* New pages must be there! */
				g_assert(NULL != pmap_lookup(vpm, narray, NULL));
			}
		} else {
			vpm->resized = TRUE;
		}
	}
}

/**
 * Add a new fragment at the tail of pmap (must be added in order), coalescing
 * fragments of the same foreign type.
 */
static void
pmap_add(struct pmap *pm, const void *start, const void *end, vmf_type_t type)
{
	struct vm_fragment *vmf;

	g_assert(pm->array != NULL);
	g_assert(pm->count <= pm->size);
	g_assert(ptr_cmp(start, end) < 0);

	mutex_get(&pm->lock);

	while (pm->count == pm->size)
		pmap_extend(pm);

	g_assert(pm->count < pm->size);

	/*
	 * Ensure that entries are inserted in order.
	 */

	if (pm->count > 0) {
		vmf = &pm->array[pm->count - 1];
		g_assert(ptr_cmp(start, vmf->end) >= 0);
	}

	/*
	 * Attempt coalescing.
	 */

	if (pm->count > 0) {
		vmf = &pm->array[pm->count - 1];
		if (vmf->type == type && vmf->end == start) {
			vmf->end = end;
			vmf->mtime = tm_time();
			goto done;
		}
	}

	vmf = &pm->array[pm->count++];
	vmf->start = start;
	vmf->end = end;
	vmf->mtime = tm_time();

done:
	mutex_release(&pm->lock);
}

/**
 * Insert region in the pmap.
 */
static void
pmap_insert_region(struct pmap *pm,
	const void *start, size_t size, vmf_type_t type)
{
	const void *end = const_ptr_add_offset(start, size);
	struct vm_fragment *vmf;
	size_t idx;
	gboolean reloaded = FALSE;

	g_assert(pm->array != NULL);
	g_assert(pm->count <= pm->size);
	g_assert(ptr_cmp(start, end) < 0);
	g_assert(round_pagesize_fast(size) == size);

	mutex_get(&pm->lock);

	/*
	 * Watch out for the kernel pmap being reloaded because the kernel did not
	 * follow our hint when the pmap pages were allocated.
	 */

	if G_UNLIKELY(pm->count == pm->size) {
		size_t generation = kernel_pmap.generation;

		pmap_extend(pm);

		if G_UNLIKELY(kernel_pmap.generation != generation) {
			if (vmm_debugging(1)) {
				s_debug("VMM kernel pmap reloaded before inserting %s [%p, %p]",
					vmf_type_str(type), start, const_ptr_add_offset(end, -1));
			}
			reloaded = TRUE;
		}

		g_assert(pm->count < pm->size);
	}

	vmf = pmap_lookup(pm, start, &idx);

	if G_UNLIKELY(vmf != NULL) {
		if (reloaded) {
			if (vmm_debugging(2)) {
				s_debug("VMM good, reloaded kernel pmap contains %s region",
					vmf_type_str(vmf->type));
			}
		} else {
			if (vmm_debugging(0)) {
				s_warning("pmap already contains new %s region [%p, %p]",
					vmf_type_str(vmf->type),
					start, const_ptr_add_offset(start, size - 1));
				vmm_dump_pmap();
			}
			g_assert(VMF_FOREIGN == type);
			g_assert_log(vmf_is_foreign(vmf),
				"vmf={%s}, start=%p, size=%zu",
				vmf_to_string(vmf), start, size);
			g_assert(ptr_cmp(end, vmf->end) <= 0);
		}
		goto done;
	} else if G_UNLIKELY(reloaded) {
		if (vmm_debugging(0)) {
			s_warning("VMM reloaded kernel pmap does not contain "
				"%s [%p, %p], will add now",
				vmf_type_str(type), start, const_ptr_add_offset(end, -1));
		}
		/* FALL THROUGH */
	}

	g_assert(idx <= pm->count);

	/*
	 * See whether we can coalesce the new region with the existing ones.
	 * We can't coalesce regions of different types.
	 */

	if (idx > 0) {
		struct vm_fragment *prev = &pm->array[idx - 1];

		g_assert_log(
			ptr_cmp(prev->end, start) <= 0, /* No overlap with prev */
			"idx=%zu, start=%p, size=%zu, prev={%s}",
			idx, start, size, vmf_to_string(prev));

		if (prev->type == type && prev->end == start) {
			prev->end = end;
			prev->mtime = tm_time();

			/*
			 * If we're now bumping into the next chunk, we need to coalesce
			 * it with the previous one and get rid of that "next" entry.
			 */
			
			if G_LIKELY(idx < pm->count) {
				struct vm_fragment *next = &pm->array[idx];

				g_assert_log(ptr_cmp(next->start, end) >= 0, /* No overlap */
					"idx=%zu, end=%p, size=%zu, next={%s}",
					idx, end, size, vmf_to_string(next));

				if (next->type == type && next->start == end) {
					prev->end = next->end;	/* prev->mtime updated above */

					/*
					 * Get rid of the entry at ``idx'' in the array.
					 * We need to shift data back by one slot unless we remove
					 * the last entry.
					 */

					pm->count--;
					if G_LIKELY(idx < pm->count) {
						memmove(&pm->array[idx], &pm->array[idx+1],
							sizeof(pm->array[0]) * (pm->count - idx));
					}
				}
			}
			goto done;
		}
	}

	if G_LIKELY(idx < pm->count) {
		struct vm_fragment *next = &pm->array[idx];

		g_assert_log(ptr_cmp(end, next->start) <= 0, /* No overlap with next */
			"idx=%zu, end=%p, size=%zu, next={%s}",
			idx, end, size, vmf_to_string(next));

		if (next->type == type && next->start == end) {
			next->start = start;
			next->mtime = tm_time();
			goto done;
		}

		/*
		 * Make room before ``idx'', the insertion point.
		 */

		memmove(&pm->array[idx+1], &pm->array[idx],
			sizeof(pm->array[0]) * (pm->count - idx));
	}

	pm->count++;
	vmf = &pm->array[idx];
	vmf->start = start;
	vmf->end = end;
	vmf->type = type;
	vmf->mtime = tm_time();

done:
	mutex_release(&pm->lock);
}

/**
 * Parse a line read from /proc/self/maps and add it to the map.
 * @return TRUE if we managed to parse the line correctly
 */
static gboolean
pmap_parse_and_add(struct pmap *pm, const char *line)
{
	int error;
	const char *ep;
	const char *p = line;
	const void *start, *end;
	gboolean foreign = FALSE;

	if (vmm_debugging(9))
		s_debug("VMM parsing \"%s\"", line);

	/*
	 * Typical format of lines on linux:
	 *
	 *                                 device
	 *  start  -  end    perm offset   mj:mn
	 *
	 * 08048000-0804f000 r-xp 00000000 09:00 1585       /bin/cat
	 * 08050000-08071000 rw-p 08050000 00:00 0          [heap]
	 * b7f9a000-b7f9d000 rw-p b7f9a000 00:00 0
	 * bfdba000-bfdcf000 rw-p bffeb000 00:00 0          [stack]
	 */

	start = parse_pointer(p, &ep, &error);
	if (error || NULL == start) {
		if (vmm_debugging(0))
			s_warning("VMM cannot parse start address");
		return FALSE;
	}

	if (*ep != '-')
		return FALSE;

	p = ++ep;
	end = parse_pointer(p, &ep, &error);
	if (error || NULL == end) {
		if (vmm_debugging(0))
			s_warning("VMM cannot parse end address");
		return FALSE;
	}

	if (ptr_cmp(end, start) <= 0) {
		if (vmm_debugging(0))
			s_warning("VMM invalid start/end address pair");
		return FALSE;
	}

	p = skip_ascii_blanks(ep);

	{
		char perms[4];
		int i;
		int c;

		for (i = 0; i < 4; i++) {
			c = *p++;
			if ('0' == c) {
				if (vmm_debugging(0))
					s_warning("VMM short permission string");
				return FALSE;
			}
			perms[i] = c;
		}

		if ('x' == perms[2] || 'w' != perms[1] || 'p' != perms[3])
			foreign = TRUE;		/* This region was not allocated by VMM */
	}

	/*
	 * FIXME: now that we have 3 types of memory region, we must recognize
	 * memory mapped regions we know about when reloading a pmap, by looking
	 * at the current map we have.	-- RAM, 2011-10-01.
	 */

	pmap_add(pm, start, end, foreign ? VMF_FOREIGN : VMF_NATIVE);

	return TRUE;
}

struct iobuffer {
	char *buf;
	size_t size;
	char *rptr;
	size_t fill;
	unsigned int eof:1;
	unsigned int error:1;
	unsigned int toobig:1;
};

static void
iobuffer_init(struct iobuffer *iob, char *dst, size_t size)
{
	iob->buf = dst;
	iob->size = size;
	iob->rptr = dst;
	iob->fill = 0;
	iob->eof = 0;
	iob->error = 0;
	iob->toobig = 0;
}

/**
 * Reads a line from an open file descriptor whereas each successive call
 * clears the previous line. Lines are separated by '\n' which is replaced
 * by a NUL when returned. The maximum acceptable line length is bound to
 * the buffer size. iob->error is set on I/O errors, iob->toobig is set
 * once the buffer is full without any newline character.
 *
 * @param iob	An initialized, valid 'struct iobuffer'.
 * @param fd	An open file descriptor.
 *
 * @return NULL on failure or EOF, pointer to the current line on success.
 */
static const char *
iobuffer_readline(struct iobuffer *iob, int fd)
{
	g_assert(iob);
	g_assert(iob->buf);
	g_assert(iob->rptr);
	g_assert(iob->fill <= iob->size);

	/* Clear previous line and shift following chars */
	if (iob->rptr != iob->buf) {
		size_t n = iob->rptr - iob->buf;

		g_assert(iob->fill >= n);
		iob->fill -= n;
		memmove(iob->buf, iob->rptr, iob->fill);
		iob->rptr = iob->buf;
	}

	do {
		/* Refill buffer if not EOF and not full */
		if (!iob->eof && iob->fill < iob->size) {
			size_t n = iob->size - iob->fill;
			ssize_t ret;

			ret = read(fd, &iob->buf[iob->fill], n);
			if ((ssize_t) -1 == ret) {
				iob->error = TRUE;
				iob->eof = TRUE;
				break;
			} else if (0 == ret) {
				iob->eof = TRUE;
			} else {
				iob->fill += ret;
			}
		}
		if (iob->fill > 0) {
			char *p = memchr(iob->buf, '\n', iob->fill);
			if (p) {
				*p = '\0';
				iob->rptr = &p[1];
				return iob->buf;
			}
		}
		if (iob->fill >= iob->size) {
			iob->toobig = TRUE;
			break;
		}
	} while (!iob->eof);

	return NULL;
}

/**
 * @return	A negative value on failure. If zero is returned, check pm->resize
 *			and try again.
 */
static inline int
pmap_load_data(struct pmap *pm)
{
	struct iobuffer iob;
	char buf[4096];
	int fd, ret = -1;

	/*
	 * This is a low-level routine, do not use stdio at all as it could
	 * allocate memory and re-enter the VMM layer.  Likewise, do not
	 * allocate any memory, excepted for the pmap where entries are stored.
	 */

	fd = open("/proc/self/maps", O_RDONLY);
	if (fd < 0) {
		if (vmm_debugging(0)) {
			s_warning("VMM cannot open /proc/self/maps: %m");
		}
		goto failure;
	}

	/*
	 * Dirty the pages associated with buf to avoid that the kernel has to
	 * extend the stack, which may modify mappings while reading from /proc.
	 */

	ZERO(&buf);

	pm->count = 0;
	iobuffer_init(&iob, buf, sizeof buf);

	while (!pm->resized) {
		const char *line;

		line = iobuffer_readline(&iob, fd);
		if (NULL == line) {
			if (iob.error) {
				if (vmm_debugging(0)) {
					s_warning("VMM error reading /proc/self/maps: %m");
				}
				goto failure;
			}
			if (iob.toobig) {
				if (vmm_debugging(0)) {
					s_warning("VMM too long a line in /proc/self/maps output");
				}
				goto failure;
			}
			break;
		}
		if (!pmap_parse_and_add(pm, line)) {
			if (vmm_debugging(0)) {
				s_warning("VMM error parsing \"%s\"", buf);
			}
			goto failure;
		}
	}
	ret = 0;

failure:
	fd_close(&fd);
	return ret;
}

/**
 * Load the process's memory map from /proc/self/maps into the supplied map.
 */
static void
pmap_load(struct pmap *pm)
{
	/*
	 * FIXME
	 *
	 * Before the 0.96.7 release, I'm disabling kernel pmap loading.
	 *
	 * This is because upon kernel map loading, we do not know currently
	 * how to propagate the regions we have allocated already to prevent
	 * them from being coalesced by the loading into a "foreign" area (despite
	 * them being truly owned by us already).
	 *
	 * This would then trigger undue assertion failures because suddenly
	 * we would think we're releasing memory allocated via vmm_mmap() (as
	 * it would be wrongly be part of a fragment tagged foreign) when in
	 * reality we're releasing memory allocated through vmm_alloc().
	 *
	 * 		--RAM, 2010-03-05
	 */
	(void) pm;

#if 0
	static int failed;
	unsigned attempt = 0;

	if (failed)
		return;

	g_assert(&kernel_pmap == pm);

	if (vmm_debugging(8)) {
		s_debug("VMM loading kernel memory map (for generation #%zu)",
			pm->generation + 1);
	}

	for (;;) {
		int ret;

		pm->loading = TRUE;
		pm->resized = FALSE;

		ret = pmap_load_data(pm);
		if (ret < 0) {
			failed = TRUE;
			break;
		}
		if (!pm->resized)
			break;

		if (++attempt > 10) {
			/* Unconditional warning -- something may break afterwards */
			s_warning("VMM unable to reload kernel pmap after %u attempts",
					attempt);
			break;
		}

		if (vmm_debugging(0)) {
			s_warning("VMM kernel pmap resized during loading attempt #%u"
				", retrying", attempt);
		}
	}

	pm->loading = FALSE;

	if (!failed)
		pm->generation++;

	if (vmm_debugging(1)) {
		s_debug("VMM kernel memory map (generation #%zu) holds %zu region%s",
			pm->generation, pm->count, 1 == pm->count ? "" : "s");
	}
#endif
}

/**
 * Log missing region.
 */
static void
pmap_log_missing(const struct pmap *pm, const void *p, size_t size)
{
	if (vmm_debugging(0)) {
		s_warning("VMM %zuKiB region at %p missing from %s pmap",
			size / 1024, p,
			pm == &kernel_pmap ? "kernel" :
			pm == &local_pmap ? "local" : "unknown");
	}
}

/**
 * Is block within an identified region, and not at the beginning or tail?
 */
static gboolean
pmap_is_within_region(const struct pmap *pm, const void *p, size_t size)
{
	struct vm_fragment *vmf;
	gboolean within;
	
	mutex_get_const(&pm->lock);

	vmf = pmap_lookup(pm, p, NULL);

	if (NULL == vmf) {
		pmap_log_missing(pm, p, size);
		return FALSE;
	}

	within = p != vmf->start && vmf->end != const_ptr_add_offset(p, size);

	mutex_release_const(&pm->lock);
	return within;
}

/**
 * Given a known-to-be-mapped block (base address and size), compute the
 * distance of its middle point to the border of the region holding it:
 * the smallest distance between the middle point and the start and end of the
 * region.
 *
 * @return the distance to the border of the enclosing region.
 */
static size_t
pmap_nesting_within_region(const struct pmap *pm, const void *p, size_t size)
{
	struct vm_fragment *vmf;
	const void *middle;
	size_t distance_to_start;
	size_t distance_to_end;

	mutex_get_const(&pm->lock);

	vmf = pmap_lookup(pm, p, NULL);

	if (NULL == vmf) {
		pmap_log_missing(pm, p, size);
		mutex_release_const(&pm->lock);
		return 0;
	}

	middle = const_ptr_add_offset(p, size / 2);
	distance_to_start = ptr_diff(middle, vmf->start);
	distance_to_end = ptr_diff(vmf->end, middle);

	mutex_release_const(&pm->lock);
	return MIN(distance_to_start, distance_to_end);
}

/**
 * Is block an identified fragment?
 */
static gboolean
pmap_is_fragment(const struct pmap *pm, const void *p, size_t npages)
{
	struct vm_fragment *vmf;
	gboolean fragment;

	mutex_get_const(&pm->lock);

	vmf = pmap_lookup(pm, p, NULL);

	if (NULL == vmf) {
		pmap_log_missing(pm, p, npages * kernel_pagesize);
		return FALSE;
	}

	fragment = p == vmf->start && npages == pagecount_fast(vmf_size(vmf));

	mutex_release_const(&pm->lock);
	return fragment;
}

/**
 * Is range available (hole) within the VM space?
 */
static gboolean
pmap_is_available(const struct pmap *pm, const void *p, size_t size)
{
	size_t idx;

	mutex_get_const(&pm->lock);

	if (pmap_lookup(pm, p, &idx)) {
		mutex_release_const(&pm->lock);
		return FALSE;
	}

	g_assert(size_is_non_negative(idx));

	if (idx < pm->count) {
		struct vm_fragment *vmf = &pm->array[idx];
		const void *end = const_ptr_add_offset(p, size);
		gboolean ret;

		ret = ptr_cmp(end, vmf->start) <= 0;
		mutex_release_const(&pm->lock);
		return ret;
	}

	mutex_release_const(&pm->lock);
	return TRUE;
}

/**
 * Assert range is within one single (coalesced) region of the VM space
 * of the specified type.
 */
static void
assert_vmm_is_allocated(const void *base, size_t size, vmf_type_t type)
{
	struct vm_fragment *vmf;
	const void *end;
	const void *vend;
	struct pmap *pm = vmm_pmap();

	g_assert(base != NULL);
	g_assert(size_is_positive(size));

	mutex_get(&pm->lock);

	vmf = pmap_lookup(vmm_pmap(), base, NULL);

	g_assert(vmf != NULL);
	g_assert(vmf_size(vmf) >= size);

	end = const_ptr_add_offset(base, size);
	vend = vmf->end;

	g_assert(ptr_cmp(base, vmf->start) >= 0);
	g_assert(ptr_cmp(end, vend) <= 0);

	/*
	 * Range was part of a single fragment, now check it is of proper type.
	 */

	g_assert(vmf->type == type);

	mutex_release(&pm->lock);
}

/**
 * Is pointer a valid native VMM one?
 */
gboolean
vmm_is_native_pointer(const void *p)
{
	struct vm_fragment *vmf;
	gboolean native;
	struct pmap *pm = vmm_pmap();

	mutex_get(&pm->lock);

	vmf = pmap_lookup(pm, page_start(p), NULL);

	native = vmf != NULL && VMF_NATIVE == vmf->type;

	mutex_release(&pm->lock);
	return native;
}

/**
 * Is region starting at ``base'' and of ``size'' bytes a virtual memory
 * fragment, i.e. a standalone mapping in the middle of the VM space?
 */
gboolean
vmm_is_fragment(const void *base, size_t size)
{
	g_assert(base != NULL);
	g_assert(size_is_positive(size));

	return pmap_is_fragment(vmm_pmap(), base, pagecount_fast(size));
}

/**
 * Is region starting at ``base'' and of ``size'' bytes a relocatable memory
 * region moveable at a better VM address?
 */
gboolean
vmm_is_relocatable(const void *base, size_t size)
{
	const void *hole;
	size_t len;

	g_assert(base != NULL);
	g_assert(size_is_positive(size));

	/*
	 * Look for a hole better placed in the VM space.
	 */
	
	hole = NULL;
	len = vmm_first_hole(&hole, FALSE);

	if (len < size)
		return FALSE;

	g_assert(hole != NULL);

	/*
	 * Don't use vmm_ptr_cmp() here since we need to correct the starting
	 * address returned by vmm_first_hole() when kernel_mapaddr_increasing
	 * is FALSE.
	 */

	if (kernel_mapaddr_increasing) {
		return ptr_cmp(hole, base) < 0;
	} else {
		hole = const_ptr_add_offset(hole, -round_pagesize_fast(size));
		return ptr_cmp(hole, base) > 0;
	}
}

/**
 * Remove whole region from the list of identified fragments.
 */
static void
pmap_remove_whole_region(struct pmap *pm, const void *p, size_t size)
{
	struct vm_fragment *vmf;
	size_t idx;

	g_assert(round_pagesize_fast(size) == size);

	mutex_get(&pm->lock);

	vmf = pmap_lookup(pm, p, NULL);

	g_assert(vmf != NULL);				/* Must be found */
	g_assert(vmf_size(vmf) == size);	/* And with the proper size */

	idx = vmf - pm->array;
	g_assert(size_is_non_negative(idx) && idx < pm->count);

	pm->count--;

	if (idx != pm->count) {
		memmove(&pm->array[idx], &pm->array[idx + 1],
			(pm->count - idx) * sizeof pm->array[0]);
	}

	mutex_release(&pm->lock);
}

/**
 * Remove region from the pmap, which can create a new fragment.
 */
static void
pmap_remove(struct pmap *pm, const void *p, size_t size)
{
	struct vm_fragment *vmf;

	g_assert(round_pagesize_fast(size) == size);

	mutex_get(&pm->lock);

	vmf = pmap_lookup(pm, p, NULL);

	if (vmf != NULL) {
		const void *end = const_ptr_add_offset(p, size);
		const void *vend = vmf->end;

		g_assert(vmf_size(vmf) >= size);

		if (p == vmf->start) {

			if (vmm_debugging(2)) {
				s_debug("VMM %s %zuKiB region at %p was %s fragment",
					vmf_type_str(vmf->type), size / 1024, p,
					end == vend ? "a whole" : "start of a");
			}

			if (end == vend) {
				pmap_remove_whole_region(pm, p, size);
			} else {
				vmf->start = end;			/* Known fragment reduced */
				vmf->mtime = tm_time();
				g_assert(ptr_cmp(vmf->start, vend) < 0);
			}
		} else {
			g_assert(ptr_cmp(vmf->start, p) < 0);
			g_assert(ptr_cmp(end, vend) <= 0);

			vmf->end = p;
			vmf->mtime = tm_time();

			if (end != vend) {
				if (vmm_debugging(1)) {
					s_debug("VMM freeing %s %zuKiB region at %p "
						"fragments VM space",
						vmf_type_str(vmf->type), size / 1024, p);
				}
				pmap_insert_region(pm, end, ptr_diff(vend, end), vmf->type);
			}
		}
	} else {
		if (vmm_debugging(0)) {
			s_warning("VMM %zuKiB region at %p missing from pmap",
				size / 1024, p);
		}
	}

	mutex_release(&pm->lock);
}

/**
 * Forcefully remove a region from the pmap (``size'' bytes starting
 * at ``p''), belonging to a given fragment.
 */
static void
pmap_remove_from(struct pmap *pm, struct vm_fragment *vmf,
	const void *p, size_t size)
{
	const void *end = const_ptr_add_offset(p, size);
	const void *vend;

	g_assert(vmf != NULL);
	g_assert(size_is_positive(size));
	g_assert(round_pagesize_fast(size) == size);

	vend = vmf->end;

	g_assert(ptr_cmp(vmf->start, p) <= 0);
	g_assert(ptr_cmp(p, vend) < 0);
	g_assert(ptr_cmp(vmf->start, end) <= 0);
	g_assert(ptr_cmp(end, vend) <= 0);

	if (vmm_debugging(0)) {
		s_warning("VMM forgetting %s %zuKiB region at %p in pmap",
			vmf_type_str(vmf->type), size / 1024, p);
	}

	if (p == vmf->start) {
		if (end == vend) {
			pmap_remove_whole_region(pm, p, size);
		} else {
			vmf->start = end;			/* Known fragment reduced */
			vmf->mtime = tm_time();
		}
	} else {
		vmf->end = p;
		vmf->mtime = tm_time();

		if (end != vend) {
			/* Insert trailing part back as a region of the same type */
			pmap_insert_region(pm, end, ptr_diff(vend, end), vmf->type);
		}
	}
}

/**
 * Kernel may have overruled our foreign region accounting by allocating
 * ``size'' bytes starting at ``p''.  Make sure we remove all foreign pages
 * in this space.
 */
static void
pmap_overrule(struct pmap *pm, const void *p, size_t size, vmf_type_t type)
{
	const void *base = p;
	size_t remain = size;

	mutex_get(&pm->lock);

	while (size_is_positive(remain)) {
		size_t idx;
		struct vm_fragment *vmf = pmap_lookup(pm, base, &idx);
		size_t len;

		g_assert(size_is_non_negative(idx));

		if (NULL == vmf) {
			const void *end = const_ptr_add_offset(base, remain);
			size_t gap;

			/* ``base'' does not belong to any known region, ``idx'' is next */

			if (idx >= pm->count)
				break;

			vmf = &pm->array[idx];

			if (ptr_cmp(end, vmf->start) <= 0) {
				mutex_release(&pm->lock);
				return;		/* Next region starts after our target */
			}

			/* We have an overlap with region in ``vmf'' */

			g_assert_log(vmf_is_foreign(vmf),
				"vmf={%s}, base=%p, remain=%zu",
				vmf_to_string(vmf), base, remain);

			gap = ptr_diff(vmf->start, base);
			g_assert(size_is_positive(gap));
			g_assert(gap <= remain);

			remain -= gap;
			base = vmf->start;

			/* FALL THROUGH */
		}

		/*
		 * We have to remove ``remain'' bytes starting at ``base''.
		 */

		vmm_stats.pmap_overruled++;
		len = ptr_diff(vmf->end, base);		/* From base to end of region */

		g_assert_log(size_is_positive(len), "len = %zu", len);

		/*
		 * When attempting an mmap() operation, we can safely overlap
		 * an existing memory-mapped region since one can mmap() a different
		 * portion of a file at the same address for instance.
		 */

		g_assert_log(VMF_MAPPED == type || vmf_is_foreign(vmf),
			"vmf={%s}, base=%p, len=%zu, remain=%zu",
			vmf_to_string(vmf), base, len, remain);

		g_assert(!vmf_is_native(vmf));	/* Never overrule allocated memory */

		if (len < remain) {
			pmap_remove_from(pm, vmf, base, len);
			base = const_ptr_add_offset(base, len);
			remain -= len;
		} else {
			pmap_remove_from(pm, vmf, base, remain);
			break;
		}
	}

	mutex_release(&pm->lock);
}

/**
 * Like free_pages() but for freeing of a cached/cacheable page and
 * update our pmap.
 */
static void
free_pages_forced(void *p, size_t size, gboolean fragment)
{
	if (vmm_debugging(fragment ? 2 : 5)) {
		s_debug("VMM freeing %zuKiB region at %p%s",
			size / 1024, p, fragment ? " (fragment)" : "");
	}

	/*
	 * Do not let free_pages_intern() update our pmap, as we are about to
	 * do it ourselves.
	 */

	free_pages_intern(p, size, FALSE);

	/*
	 * If we're freeing a fragment, we can remove it from the list of known
	 * regions, since we already know it is a standalone region.
	 */

	if (fragment) {
		pmap_remove_whole_region(vmm_pmap(), p, size);
	} else {
		pmap_remove(vmm_pmap(), p, size);
	}
}

/**
 * Lookup page within a cache line.
 *
 * If ``low_ptr'' is non-NULL, it is written with the index where insertion
 * of a new item should happen (in which case the returned value must be -1).
 *
 * @return index within the page cache info[] sorted array where "p" is stored,
 * -1 if not found.
 */
static size_t
vpc_lookup(const struct page_cache *pc, const char *p, size_t *low_ptr)
{
	size_t low = 0, high = pc->current - 1;
	size_t mid;

	/* Binary search */

	for (;;) {
		const char *item;

		if (low > high || high > SIZE_MAX / 2) {
			mid = -1;		/* Not found */
			break;
		}

		mid = low + (high - low) / 2;

		g_assert(mid < pc->current);

		item = pc->info[mid].base;
		if (p > item)
			low = mid + 1;
		else if (p < item)
			high = mid - 1;
		else
			break;				/* Found */
	}

	if (low_ptr != NULL)
		*low_ptr = low;

	return mid;
}

/**
 * Remove entry within a cache line at specified index.
 * The associated pages are not released to the system.
 */
static void
vpc_remove_at(struct page_cache *pc, const void *p, size_t idx)
{
	g_assert(size_is_non_negative(idx) && idx < pc->current);
	g_assert(p == pc->info[idx].base);
	assert_vmm_is_allocated(p, pc->chunksize, VMF_NATIVE);
	g_assert(size_is_positive(pc->current));
	g_assert(spinlock_is_held(&pc->lock));

	/*
	 * Delete the slot entry, moving back items by one position unless the
	 * last item was removed.
	 */

	pc->current--;
	if (idx < pc->current) {
		memmove(&pc->info[idx], &pc->info[idx + 1],
			(pc->current - idx) * sizeof(pc->info[0]));
	}
}

/**
 * Remove entry within a cache line, keeping the associated pages mapped.
 */
static void
vpc_remove(struct page_cache *pc, const void *p)
{
	size_t idx;

	g_assert(spinlock_is_held(&pc->lock));

	idx = vpc_lookup(pc, p, NULL);
	g_assert(idx != (size_t) -1);		/* Must have been found */
	vpc_remove_at(pc, p, idx);
}

/**
 * Free page cached at given index in cache line.
 */
static void
vpc_free(struct page_cache *pc, size_t idx)
{
	void *p;

	g_assert(size_is_non_negative(idx) && idx < pc->current);
	g_assert(spinlock_is_held(&pc->lock));

	p = pc->info[idx].base;
	vpc_remove_at(pc, p, idx);
	free_pages_forced(p, pc->chunksize, FALSE);
}

/**
 * Insert entry within a cache line, coalescing consecutive entries in
 * higher-order cache lines, recursively.
 *
 * The base pointer MUST be pointing to a memory region corresponding to
 * the size of the chunks held in the cache.
 *
 * @param pc	page cache where entry is inserted
 * @param p		base pointer of the region to be added
 */
static void
vpc_insert(struct page_cache *pc, void *p)
{
	size_t idx;
	void *base = p;
	size_t pages = pc->pages;

	g_assert(size_is_non_negative(pc->current) &&
		pc->current <= VMM_CACHE_SIZE);

	spinlock(&pc->lock);

	if G_UNLIKELY((size_t ) -1 != vpc_lookup(pc, p, &idx)) {
		s_error_from(_WHERE_, "memory chunk at %p already present in cache", p);
	}

	g_assert(size_is_non_negative(idx) && idx <= pc->current);
	assert_vmm_is_allocated(p, pc->chunksize, VMF_NATIVE);

	/*
	 * If we're inserting in the highest-order cache, there's no need
	 * to attempt to do any coalescing.
	 */

	if (VMM_CACHE_LINES == pages)
		goto insert;

	/*
	 * Look whether the page chunk before is also present in the cache line.
	 * If it is, it can be removed and coalesced with the current chunk.
	 */

	if (idx > 0) {
		void *before = pc->info[idx - 1].base;
		void *bend = ptr_add_offset(before, pc->chunksize);

		if G_UNLIKELY(bend == p) {
			if (vmm_debugging(6)) {
				s_debug("VMM page cache #%zu: "
					"coalescing previous [%p, %p] with [%p, %p]",
					pc->pages - 1, before, ptr_add_offset(bend, -1), p,
					ptr_add_offset(p, pc->chunksize - 1));
			}
			base = before;
			pages += pc->pages;
			vpc_remove_at(pc, before, idx - 1);
			idx--;		/* Removed entry before insertion point */
		}
	}

	/*
	 * Look whether the page chunk after is also present in the cache.
	 * If it is, it can be removed and coalesced with the current chunk.
	 */

	if (idx < pc->current) {
		void *end = ptr_add_offset(p, pc->chunksize);
		void *next = pc->info[idx].base;

		if G_UNLIKELY(next == end) {
			if (vmm_debugging(6)) {
				s_debug("VMM page cache #%zu: "
					"coalescing [%p, %p] with next [%p, %p]",
					pc->pages - 1, base, ptr_add_offset(end, -1), next,
					ptr_add_offset(next, pc->chunksize - 1));
			}
			pages += pc->pages;
			vpc_remove_at(pc, next, idx);
		}
	}

	/*
	 * If we coalesced the chunk, recurse to insert it in the proper cache.
	 * Otherwise, insertion is happening before ``idx''.
	 */

	if G_UNLIKELY(pages != pc->pages) {
		if (vmm_debugging(2)) {
			s_debug("VMM coalesced %zuKiB region [%p, %p] into "
				"%zuKiB region [%p, %p] (recursing)",
				pc->chunksize / 1024,
				p, ptr_add_offset(p, pc->chunksize - 1),
				pages * kernel_pagesize / 1024,
				base, ptr_add_offset(base, pages * kernel_pagesize - 1));
		}
		spinunlock(&pc->lock);
		page_cache_insert_pages(base, pages);
		vmm_stats.cache_line_coalescing++;
		return;
	}

insert:

	/*
	 * Insert region in the local cache line.
	 */

	if G_UNLIKELY(VMM_CACHE_SIZE == pc->current) {
		size_t kidx = kernel_mapaddr_increasing ? VMM_CACHE_SIZE - 1 : 0;
		if (vmm_debugging(4)) {
			void *lbase = pc->info[kidx].base;
			s_debug("VMM page cache #%zu: kicking out [%p, %p] %zuKiB",
				pc->pages - 1, lbase, ptr_add_offset(lbase, pc->chunksize - 1),
				pc->chunksize / 1024);
		}
		vpc_free(pc, kidx);
		if (idx > kidx)
			idx--;
		vmm_stats.cache_evictions++;
	}

	g_assert(size_is_non_negative(pc->current) && pc->current < VMM_CACHE_SIZE);
	g_assert(size_is_non_negative(idx) && idx < VMM_CACHE_SIZE);

	/*
	 * Shift items up if we're not inserting at the last position in the array.
	 */

	if G_LIKELY(idx < pc->current) {
		memmove(&pc->info[idx + 1], &pc->info[idx],
			(pc->current - idx) * sizeof(pc->info[0]));
	}

	pc->current++;
	pc->info[idx].base = base;
	pc->info[idx].stamp = tm_time();

	if (vmm_debugging(4)) {
		s_debug("VMM page cache #%zu: inserted [%p, %p] %zuKiB, "
			"now holds %zu item%s",
			pc->pages - 1, base, ptr_add_offset(base, pc->chunksize - 1),
			pc->chunksize / 1024, pc->current, 1 == pc->current ? "" : "s");
	}

	spinunlock(&pc->lock);
}

/**
 * Compare two pointers according to the growing direction of the VM space.
 * A pointer is larger than another if it is further away from the base.
 */
static inline int
vmm_ptr_cmp(const void *a, const void *b)
{
	if (a == b)
		return 0;

	return (kernel_mapaddr_increasing ? +1 : -1) * ptr_cmp(a, b);
}

/**
 * Find "n" consecutive pages in page cache and remove them from it.
 *
 * @param pc		the page cache line to search
 * @param n			amount of pages wanted
 * @param hole		if non-NULL, first known hole (uncached) that would fit
 *
 * @return pointer to the base of the "n" pages if found, NULL otherwise.
 */
static void *
vpc_find_pages(struct page_cache *pc, size_t n, const void *hole)
{
	size_t total = 0;
	void *base, *end;

	spinlock(&pc->lock);

	if (0 == pc->current)
		goto not_found;

	if (n > pc->pages) {
		size_t i;

		/*
		 * Since we're looking for more pages than what this cache line stores,
		 * we'll have to coalesce the page with neighbouring chunks, which
		 * only makes sense when looking for pages in the highest order page
		 * cache.
		 */

		for (i = 0; i < pc->current; i++) {
			base = kernel_mapaddr_increasing ?
				pc->info[i].base : pc->info[pc->current - 1 - i].base;

			/*
			 * We stop considering pages that are further away than the
			 * identified hole.  Since we traverse the cache line in the
			 * proper order, from "lower" addresses to "upper" ones, we can
			 * abort as soon as we've gone beyond the hole.
			 */

			if (hole != NULL && vmm_ptr_cmp(base, hole) > 0) {
				if (vmm_debugging(7)) {
					s_debug("VMM page cache #%zu: stopping lookup attempt "
						"for %zu page%s at %p (upper than hole %p)",
						pc->pages - 1, n, 1 == n ? "" : "s", base, hole);
				}
				break;
			}

			goto selected;
		}

		goto not_found;			/* Cache line was empty */
	} else {
		size_t i;
		size_t max_distance = 0;
		base = NULL;

		/*
		 * We're looking for less consecutive pages than what this cache
		 * line holds, which will require splitting of the entry.
		 *
		 * Allocate the innermost pages within the region, so that pages at
		 * the beginning or end of the region remain unused and can be
		 * released if they're not needed.
		 */

		for (i = 0; i < pc->current; i++) {
			size_t d;
			const void *p;

			p = kernel_mapaddr_increasing ?
				pc->info[i].base : pc->info[pc->current - 1 - i].base;

			/*
			 * Stop considering pages that are further away from the
			 * identified hole.
			 */

			if (hole != NULL && vmm_ptr_cmp(p, hole) > 0) {
				if (vmm_debugging(7)) {
					s_debug("VMM page cache #%zu: "
						"stopping lookup for %zu page%s at %p "
						"(upper than hole %p)",
						pc->pages - 1, n, 1 == n ? "" : "s", p, hole);
				}
				break;
			}

			d = pmap_nesting_within_region(vmm_pmap(), p, pc->chunksize);
			if (d > max_distance) {
				max_distance = d;
				base = deconstify_gpointer(p);
			}
		}

		if (NULL == base)
			goto not_found;		/* Cache line was empty */

		/* FALL THROUGH */
	}

selected:
	end = ptr_add_offset(base, pc->chunksize);
	total = pc->pages;

	/*
	 * If we have not yet the amount of pages we want, iterate to find
	 * whether we have consecutive ranges in the cache.  This is only
	 * necessary if we are already in the highest-order cache line because
	 * lower-order caches are coalesced at insertion time.
	 */

	if (total < n) {
		size_t i;

		if (VMM_CACHE_LINES != pc->pages)
			goto not_found;		/* Not at highest-order cache line */

		if (1 == pc->current)
			goto not_found;		/* No other entries to coalesce with */

		if (kernel_mapaddr_increasing) {
			for (i = 1; i < pc->current; i++) {
				void *start = pc->info[i].base;

				if (start == end) {
					total += pc->pages;
					end = ptr_add_offset(start, pc->chunksize);
					if (total >= n)
						goto found;
				} else {
					if (hole != NULL && vmm_ptr_cmp(start, hole) > 0) {
						if (vmm_debugging(7)) {
							s_debug("VMM cache #%zu: stopping merge for "
								"%zu page%s (had %zu already) at %p "
								"(upper than hole %p)",
								pc->pages - 1, n, 1 == n ? "" : "s",
								total, start, hole);
						}
						break;
					}

					/*
					 * No luck coalescing what we had so far with the next
					 * entry.  Restart the coalescing process from this
					 * cached entry then.
					 */

					total = pc->pages;
					base = start;
					end = ptr_add_offset(base, pc->chunksize);
				}
			}
		} else {
			for (i = pc->current - 1; i > 0; i--) {
				void *prev_base = pc->info[i - 1].base;
				void *last = ptr_add_offset(prev_base, pc->chunksize);

				if (last == base) {
					total += pc->pages;
					base = prev_base;
					if (total >= n)
						goto found;
				} else {
					if (hole != NULL && vmm_ptr_cmp(prev_base, hole) > 0) {
						if (vmm_debugging(7)) {
							s_debug("VMM cache #%zu: stopping merge for "
								"%zu page%s (had %zu already) at %p "
								"(upper than hole %p)",
								pc->pages - 1, n, 1 == n ? "" : "s",
								total, prev_base, hole);
						}
						break;
					}

					/*
					 * No luck coalescing what we had so far with the next
					 * entry.  Restart the coalescing process from this
					 * cached entry then.
					 */

					total = pc->pages;
					base = prev_base;
					end = last;
				}
			}
		}

		goto not_found;		/* Did not find enough consecutive pages */
	}

found:
	g_assert(total >= n);

	/*
	 * Remove the entries from the cache.
	 */

	{
		void *p = base;
		size_t i;

		for (i = 0; i < total; i += pc->pages) {
			vpc_remove(pc, p);
			p = ptr_add_offset(p, pc->chunksize);
		}

		g_assert(end == p);
	}

	spinunlock(&pc->lock);

	/*
	 * If we got more consecutive pages than the amount asked for, put
	 * the leading / trailing pages back into the cache.
	 */

	if (total > n) {
		if (kernel_mapaddr_increasing) {
			void *start = ptr_add_offset(base, n * kernel_pagesize);
			page_cache_insert_pages(start, total - n);	/* Strip trailing */
		} else {
			void *start = ptr_add_offset(end, -n * kernel_pagesize);
			page_cache_insert_pages(base, total - n);	/* Strip leading */
			base = start;
		}
	}

	return base;

not_found:
	spinunlock(&pc->lock);
	return NULL;
}

/**
 * Find "n" consecutive pages in the page cache, and remove them if found.
 *
 * @param n			number of pages we want
 * @param hole_ptr	variable where first hole we found is written to
 *
 * @return a pointer to the start of the memory region, NULL if we were
 * unable to find that amount of contiguous memory.  In any case, ``hole_ptr''
 * is written with the first suitable hole we identified in the VM space.
 */
static void *
page_cache_find_pages(size_t n, const void **hole_ptr)
{
	void *p;
	size_t len;
	const void *hole;
	struct page_cache *pc = NULL;

	g_assert(size_is_positive(n));
	g_assert(hole_ptr != NULL);

	/*
	 * Before using pages from the cache, look where the first hole is
	 * at the start of the virtual memory space.  If we find one that can
	 * suit this allocation, then do not use cached pages that would
	 * lie after the hole.
	 */

	hole = NULL;
	len = vmm_first_hole(&hole, TRUE);

	if (pagecount_fast(len) < n) {
		hole = NULL;
	} else if (!kernel_mapaddr_increasing) {
		size_t length = size_saturate_mult(n, kernel_pagesize);

		g_assert((size_t) -1 != length);
		g_assert(ptr_diff(hole, NULL) > length);

		hole = const_ptr_add_offset(hole, -length);
	}

	if G_UNLIKELY(hole != NULL) {
		if (vmm_debugging(8)) {
			size_t np = pagecount_fast(len);
			s_debug("VMM lowest hole of %zu page%s at %p (%zu page%s)",
				n, 1 == n ? "" : "s", hole, np, 1 == np ? "" : "s");
		}
	}

	*hole_ptr = hole;

	if (n >= VMM_CACHE_LINES) {
		/*
		 * Our only hope to find suitable pages in the cache is to have
		 * consecutive ranges in the largest cache line.
		 */

		pc = &page_cache[VMM_CACHE_LINES - 1];
		p = vpc_find_pages(pc, n, hole);

		/*
		 * Count when we coalesce large blocks from the higher cache line.
		 */

		if (p != NULL && n > VMM_CACHE_LINES)
			vmm_stats.high_order_coalescing++;

		if (vmm_debugging(3)) {
			s_debug("VMM lookup for large area (%zu pages) returned %p", n, p);
		}
	} else {
		/*
		 * Start looking for a suitable page in the page cache matching
		 * the size we want.
		 */

		pc = &page_cache[n - 1];
		p = vpc_find_pages(pc, n, hole);

		/*
		 * Visit higher-order cache lines if we found nothing in the cache.
		 *
		 * To avoid VM space fragmentation, we never split a larger region
		 * to allocate just one page.  This policy allows us to fill the holes
		 * that can be created and avoid undoing the coalescing we may have
		 * achieved so far in higher-order caches.
		 */

		if (NULL == p && n > 1) {
			size_t i;

			for (i = n; i < VMM_CACHE_LINES && NULL == p; i++) {
				pc = &page_cache[i];
				p = vpc_find_pages(pc, n, hole);
			}
		}
	}

	if (p != NULL) {
		if (vmm_debugging(5)) {
			s_debug("VMM found %zuKiB region at %p in cache #%zu%s",
				n * kernel_pagesize / 1024, p, pc->pages - 1,
				pc->pages == n ? "" :
				n > VMM_CACHE_LINES ? " (merged)" : " (split)");
		}
	}

	return p;
}

/**
 * Insert "n" consecutive pages starting at "base" in the page cache.
 *
 * If the cache is full, older memory is released and the new one is kept,
 * so that we minimize the risk of having cached memory swapped out.
 *
 * @return TRUE if pages were cached, FALSE if they were forcefully freed.
 */
static gboolean
page_cache_insert_pages(void *base, size_t n)
{
	size_t pages = n;
	void *p = base;

	assert_vmm_is_allocated(base, n * kernel_pagesize, VMF_NATIVE);

	/*
	 * Identified memory fragments are immediately freed and not put
	 * back into the cache, in order to reduce fragmentation of the
	 * memory space.
	 */

	if (pmap_is_fragment(vmm_pmap(), base, n)) {
		free_pages_forced(base, n * kernel_pagesize, TRUE);
		vmm_stats.forced_freed++;
		vmm_stats.forced_freed_pages += n;
		return FALSE;
	}

	/*
	 * If releasing more than what we can store in the largest cache line,
	 * break up the region into smaller chunks.
	 */

	while (pages > VMM_CACHE_LINES) {
		struct page_cache *pc = &page_cache[VMM_CACHE_LINES - 1];

		vpc_insert(pc, p);
		pages -= pc->pages;
		p = ptr_add_offset(p, pc->chunksize);
	}

	if (size_is_positive(pages)) {
		g_assert(pages <= VMM_CACHE_LINES);
		vpc_insert(&page_cache[pages - 1], p);
	}

	return TRUE;
}

/**
 * Attempting to coalesce the block with other entries in the cache.
 *
 * Cached pages being coalesced with the block are removed from the cache
 * and the resulting block is not part of the cache.
 *
 * @return TRUE if coalescing occurred, with updated base and amount of pages.
 */
static gboolean
page_cache_coalesce_pages(void **base_ptr, size_t *pages_ptr)
{
	size_t i, j;
	void *base = *base_ptr;
	size_t pages = *pages_ptr;
	const size_t old_pages = pages;
	void *end;

	assert_vmm_is_allocated(base, pages * kernel_pagesize, VMF_NATIVE);

	if (pages >= VMM_CACHE_LINES)
		return FALSE;

	end = ptr_add_offset(base, pages * kernel_pagesize);

	/*
	 * Look in low-order caches whether we can find chunks before.
	 */

	for (i = 0; /* empty */; i++) {
		gboolean coalesced = FALSE;

		j = MIN(old_pages, VMM_CACHE_LINES) - 1;
		while (j-- > 0) {
			struct page_cache *lopc = &page_cache[j];
			void *before;
			size_t loidx;

			spinlock(&lopc->lock);

			if (0 == lopc->current) {
				spinunlock(&lopc->lock);
				continue;
			}

			before = ptr_add_offset(base, -lopc->chunksize);
			loidx = vpc_lookup(lopc, before, NULL);

			if G_UNLIKELY(loidx != (size_t) -1) {
				if (vmm_debugging(6)) {
					s_debug("VMM iter #%zu, coalescing previous "
						"[%p, %p] from lower cache #%zu with [%p, %p]",
						i, before, ptr_add_offset(base, -1),
						lopc->pages - 1, base,
						ptr_add_offset(base, pages * kernel_pagesize - 1));
				}
				assert_vmm_is_allocated(before,
					(pages + lopc->pages) * kernel_pagesize, VMF_NATIVE);
				base = before;
				pages += lopc->pages;
				vpc_remove_at(lopc, before, loidx);
				coalesced = TRUE;
				spinunlock(&lopc->lock);
				if (pages >= VMM_CACHE_LINES)
					goto done;
			} else {
				spinunlock(&lopc->lock);
			}
		}

		if (!coalesced)
			break;
	}

	/**
	 * Look in higher-order caches whether we can find chunks before.
	 * Stop before the highest-order cache since regions there cannot be
	 * coalesced with any other (inserting them back to the cache would split
	 * the ranges again).
	 */

	for (j = old_pages; j < VMM_CACHE_LINES - 1; j++) {
		struct page_cache *hopc = &page_cache[j];
		void *before;
		size_t hoidx;

		spinlock(&hopc->lock);

		if (0 == hopc->current) {
			spinunlock(&hopc->lock);
			continue;
		}

		before = ptr_add_offset(base, -hopc->chunksize);
		hoidx = vpc_lookup(hopc, before, NULL);

		if G_UNLIKELY(hoidx != (size_t) -1) {
			if (vmm_debugging(6)) {
				s_debug("VMM coalescing previous [%p, %p] "
					"from higher cache #%zu with [%p, %p]",
					before, ptr_add_offset(base, -1), hopc->pages - 1,
					base, ptr_add_offset(base, pages * kernel_pagesize - 1));
			}
			assert_vmm_is_allocated(before,
				(pages + hopc->pages) * kernel_pagesize, VMF_NATIVE);
			base = before;
			pages += hopc->pages;
			vpc_remove_at(hopc, before, hoidx);
			spinunlock(&hopc->lock);
			if (pages >= VMM_CACHE_LINES)
				goto done;
		} else {
			spinunlock(&hopc->lock);
		}
	}

	/*
	 * Look in low-order caches whether we can find chunks after.
	 */

	g_assert(ptr_add_offset(base, pages * kernel_pagesize) == end);

	for (i = 0; /* empty */; i++) {
		gboolean coalesced = FALSE;

		j = MIN(old_pages, VMM_CACHE_LINES) - 1;
		while (j-- > 0) {
			struct page_cache *lopc = &page_cache[j];
			size_t loidx;

			spinlock(&lopc->lock);

			if (0 == lopc->current) {
				spinunlock(&lopc->lock);
				continue;
			}

			loidx = vpc_lookup(lopc, end, NULL);

			if G_UNLIKELY(loidx != (size_t) -1) {
				if (vmm_debugging(6)) {
					s_debug("VMM iter #%zu, coalescing next [%p, %p] "
						"from lower cache #%zu with [%p, %p]",
						i, end, ptr_add_offset(end, lopc->chunksize - 1),
						lopc->pages - 1, base, ptr_add_offset(end, -1));
				}
				assert_vmm_is_allocated(base,
					(pages + lopc->pages) * kernel_pagesize, VMF_NATIVE);
				pages += lopc->pages;
				vpc_remove_at(lopc, end, loidx);
				end = ptr_add_offset(end, lopc->chunksize);
				spinunlock(&lopc->lock);
				coalesced = TRUE;
				if (pages >= VMM_CACHE_LINES)
					goto done;
			} else {
				spinunlock(&lopc->lock);
			}
		}

		if (!coalesced)
			break;
	}

	/**
	 * Look in higher-order caches whether we can find chunks after.
	 * Stop before the highest-order cache since regions there cannot be
	 * coalesced with any other (inserting them back to the cache would split
	 * the ranges again).
	 */

	for (j = old_pages; j < VMM_CACHE_LINES - 1; j++) {
		struct page_cache *hopc = &page_cache[j];
		size_t hoidx;

		spinlock(&hopc->lock);

		if (0 == hopc->current) {
			spinunlock(&hopc->lock);
			continue;
		}

		hoidx = vpc_lookup(hopc, end, NULL);

		if G_UNLIKELY(hoidx != (size_t) -1) {
			if (vmm_debugging(6)) {
				s_debug("VMM coalescing next [%p, %p] "
					"from higher cache #%zu with [%p, %p]",
					end, ptr_add_offset(end, hopc->chunksize - 1),
					hopc->pages - 1, base, ptr_add_offset(end, -1));
			}
			assert_vmm_is_allocated(base,
				(pages + hopc->pages) * kernel_pagesize, VMF_NATIVE);
			pages += hopc->pages;
			vpc_remove_at(hopc, end, hoidx);
			end = ptr_add_offset(end, hopc->chunksize);
			spinunlock(&hopc->lock);
			if (pages >= VMM_CACHE_LINES)
				goto done;
		} else {
			spinunlock(&hopc->lock);
		}
	}

done:
	assert_vmm_is_allocated(base, pages * kernel_pagesize, VMF_NATIVE);
	g_assert(ptr_add_offset(base, pages * kernel_pagesize) == end);

	if G_UNLIKELY(pages != old_pages) {
		if (vmm_debugging(2)) {
			s_debug("VMM coalesced %zuKiB region [%p, %p] into "
				"%zuKiB region [%p, %p]",
				old_pages * kernel_pagesize / 1024, *base_ptr,
				ptr_add_offset(*base_ptr, old_pages * compat_pagesize() - 1),
				pages * kernel_pagesize / 1024,
				base, ptr_add_offset(base, pages * kernel_pagesize - 1));
		}

		*base_ptr = base;
		*pages_ptr = pages;
		vmm_stats.cache_coalescing++;

		return TRUE;
	}

	return FALSE;
}

void
vmm_madvise_normal(void *p, size_t size)
{
	g_assert(p);
	g_assert(size_is_positive(size));
#if defined(HAS_MADVISE) && defined(MADV_NORMAL)
	madvise(p, size, MADV_NORMAL);
#endif	/* MADV_NORMAL */
}

void
vmm_madvise_sequential(void *p, size_t size)
{
	g_assert(p);
	g_assert(size_is_positive(size));
#if defined(HAS_MADVISE) && defined(MADV_SEQUENTIAL)
	madvise(p, size, MADV_SEQUENTIAL);
#endif	/* MADV_SEQUENTIAL */
}

void
vmm_madvise_free(void *p, size_t size)
{
	g_assert(p);
	g_assert(size_is_positive(size));
#if defined(HAS_MADVISE) && defined(MADV_FREE)
	madvise(p, size, MADV_FREE);
#elif defined(HAS_MADVISE) && defined(MADV_DONTNEED)
	madvise(p, size, MADV_DONTNEED);
#endif	/* MADV_FREE */
}

void
vmm_madvise_willneed(void *p, size_t size)
{
	g_assert(p);
	g_assert(size_is_positive(size));
#if defined(HAS_MADVISE) && defined(MADV_WILLNEED)
	madvise(p, size, MADV_WILLNEED);
#endif	/* MADV_WILLNEED */
}

static void
vmm_validate_pages(void *p, size_t size)
{
	g_assert(p);
	g_assert(size_is_positive(size));
#ifdef VMM_PROTECT_FREE_PAGES
	mprotect(p, size, PROT_READ | PROT_WRITE);
#endif	/* VMM_PROTECT_FREE_PAGES */
#ifdef VMM_INVALIDATE_FREE_PAGES 
	/*  This should be unnecessary */
	/*  vmm_madvise_normal(p, size); */
#endif	/* VMM_INVALIDATE_FREE_PAGES */
}

static void
vmm_invalidate_pages(void *p, size_t size)
{
	g_assert(p);
	g_assert(size_is_positive(size));

	if (G_UNLIKELY(stop_freeing))
		return;

#ifdef VMM_PROTECT_FREE_PAGES
	mprotect(p, size, PROT_NONE);
#endif	/* VMM_PROTECT_FREE_PAGES */
#ifdef VMM_INVALIDATE_FREE_PAGES 
	vmm_madvise_free(p, size);
#endif	/* VMM_INVALIDATE_FREE_PAGES */
}

/**
 * Allocates a page-aligned memory chunk, possibly returning a cached region
 * and only allocating a new region when necessary.
 *
 * When ``user_mem'' is FALSE, it means we're allocating memory that will be
 * used by other memory allocators, not by "users": that memory will be
 * accounted for when it is in turn allocated for user consumption, which is
 * why we must not account for it here as being user memory.
 *
 * @param size 		size in bytes to allocate; will be rounded to the pagesize.
 * @param user_mem	whether this memory is meant for "user" consumption
 *
 * @return pointer to allocated memory region
 */
static void *
vmm_alloc_internal(size_t size, gboolean user_mem)
{
	size_t n;
	void *p;
	const void *hole;

	if G_UNLIKELY(0 == kernel_pagesize)
		vmm_init(&n);

	g_assert(size_is_positive(size));

	size = round_pagesize_fast(size);
	n = pagecount_fast(size);
	vmm_stats.allocations++;

	/*
	 * First look in the page cache to avoid requesting a new memory
	 * mapping from the kernel.
	 */

	p = page_cache_find_pages(n, &hole);
	if (p != NULL) {
		vmm_validate_pages(p, size);
		assert_vmm_is_allocated(p, size, VMF_NATIVE);

		vmm_stats.alloc_from_cache++;
		vmm_stats.alloc_from_cache_pages += n;
		goto update_stats;
	}

	p = alloc_pages(size, TRUE, hole);
	if (NULL == p)
		s_error("cannot allocate %zu bytes: out of virtual memory", size);

	assert_vmm_is_allocated(p, size, VMF_NATIVE);
	vmm_stats.alloc_direct_core++;
	vmm_stats.alloc_direct_core_pages += n;

	/* FALL THROUGH */

update_stats:
	if (user_mem) {
		vmm_stats.user_memory += size;
		vmm_stats.user_pages += n;
		vmm_stats.user_blocks++;
		memusage_add(vmm_stats.user_mem, size);
	} else {
		vmm_stats.core_memory += size;
		vmm_stats.core_pages += n;
		memusage_add(vmm_stats.core_mem, size);
	}

	return p;
}

/**
 * Allocates a page-aligned memory chunk, possibly returning a cached region
 * and only allocating a new region when necessary.
 *
 * @param size The size in bytes to allocate; will be rounded to the pagesize.
 */
void *
vmm_alloc(size_t size)
{
	return vmm_alloc_internal(size, TRUE);
}

/**
 * Allocates a page-aligned memory chunk, meant to be use as core for other
 * memory allocator built on top of this layer.
 *
 * This means memory allocated here will NOT be accounted for as "user memory"
 * and it MUST be freed with vmm_core_free() for sound accounting.
 *
 * @param size The size in bytes to allocate; will be rounded to the pagesize.
 */
void *
vmm_core_alloc(size_t size)
{
	return vmm_alloc_internal(size, FALSE);
}

/**
 * Same a vmm_alloc() but zeroes the allocated region.
 */
void *
vmm_alloc0(size_t size)
{
	size_t n;
	void *p;
	const void *hole;

	if G_UNLIKELY(0 == kernel_pagesize)
		vmm_init(&n);

	g_assert(size_is_positive(size));

	size = round_pagesize_fast(size);
	n = pagecount_fast(size);
	vmm_stats.allocations++;
	vmm_stats.allocations_zeroed++;

	/*
	 * First look in the page cache to avoid requesting a new memory
	 * mapping from the kernel.
	 */

	p = page_cache_find_pages(n, &hole);
	if (p != NULL) {
		vmm_validate_pages(p, size);
		memset(p, 0, size);
		assert_vmm_is_allocated(p, size, VMF_NATIVE);

		vmm_stats.alloc_from_cache++;
		vmm_stats.alloc_from_cache_pages += n;
		goto update_stats;
	}

	p = alloc_pages(size, TRUE, hole);
	if (NULL == p)
		s_error("cannot allocate %zu bytes: out of virtual memory", size);

	/* Memory allocated by the kernel is already zero-ed */

	assert_vmm_is_allocated(p, size, VMF_NATIVE);
	vmm_stats.alloc_direct_core++;
	vmm_stats.alloc_direct_core_pages += n;

	/* FALL THROUGH */

update_stats:

	/*
	 * Always allocating "user" memory: since "core" memory does not need
	 * to be zeroed, which is why there is no vmm_core_alloc0().
	 */

	vmm_stats.user_memory += size;
	vmm_stats.user_pages += n;
	vmm_stats.user_blocks++;
	memusage_add(vmm_stats.user_mem, size);

	return p;
}

/**
 * Free memory allocated via vmm_alloc() or vmm_core_alloc(), possibly
 * returning it to the cache.
 *
 * When ``user_mem'' is FALSE, it means we're freeing memory that was being
 * used by other memory allocators, not by "users": that memory was not
 * accounted for as being used by this layer but rather as "core" memory,
 * i.e. resource for other memory allocators.
 */
static void
vmm_free_internal(void *p, size_t size, gboolean user_mem)
{
	g_assert(0 == size || p);
	g_assert(size_is_non_negative(size));

	if (p) {
		size_t n;

		g_assert(page_start(p) == p);

		size = round_pagesize_fast(size);
		n = pagecount_fast(size);
		vmm_stats.freeings++;

		g_assert(n >= 1);			/* Asserts that size != 0 */

		assert_vmm_is_allocated(p, size, VMF_NATIVE);

		/*
		 * Memory regions that are larger than our highest-order cache
		 * are allocated and freed as-is, and never broken into smaller
		 * pages.  The purpose is to avoid excessive virtual memory space
		 * fragmentation, leading at some point to the unability for the
		 * kernel to find a large consecutive virtual memory region.
		 */

		if (n <= VMM_CACHE_LINES) {
			size_t m = n;
			vmm_invalidate_pages(p, size);
			page_cache_coalesce_pages(&p, &m);
			if (page_cache_insert_pages(p, m)) {
				vmm_stats.free_to_cache++;
				vmm_stats.free_to_cache_pages += n;
			}
		} else {
			free_pages(p, size, TRUE);
			vmm_stats.free_to_system++;
			vmm_stats.free_to_system_pages += n;
		}

		if (user_mem) {
			vmm_stats.user_memory -= size;
			vmm_stats.user_pages -= n;
			g_assert(size_is_non_negative(vmm_stats.user_pages));
			g_assert(size_is_non_negative(vmm_stats.user_memory));
			vmm_stats.user_blocks--;
			memusage_remove(vmm_stats.user_mem, size);
		} else {
			vmm_stats.core_memory -= size;
			vmm_stats.core_pages -= n;
			g_assert(size_is_non_negative(vmm_stats.core_pages));
			g_assert(size_is_non_negative(vmm_stats.core_memory));
			memusage_remove(vmm_stats.core_mem, size);
		}
	}
}

/**
 * Free memory allocated via vmm_alloc(), possibly returning it to the cache.
 */
void
vmm_free(void *p, size_t size)
{
	vmm_free_internal(p, size, TRUE);
}

/**
 * Free core allocated via vmm_core_alloc(), possibly returning it to the cache.
 */
void
vmm_core_free(void *p, size_t size)
{
	vmm_free_internal(p, size, FALSE);
}

/**
 * Shrink allocated space via vmm_alloc() or vmm_core_alloc() down to
 * specified size.
 *
 * @param p			the memory region base
 * @param size		current size of the memory region
 * @param new_size	new requested size of the memory region (smaller)
 * @param user_mem	whether this was memory used directly by callers
 *
 * Calling with a NULL pointer is OK when the size is 0.
 * Otherwise calling with a new_size of 0 actually frees the memory region.
 */
static void
vmm_shrink_internal(void *p, size_t size, size_t new_size, gboolean user_mem)
{
	g_assert(0 == size || p != NULL);
	g_assert(new_size <= size);
	g_assert(page_start(p) == p);

	if (0 == new_size) {
		vmm_free_internal(p, size, user_mem);
	} else if (p != NULL) {
		size_t osize, nsize;

		assert_vmm_is_allocated(p, size, VMF_NATIVE);

		osize = round_pagesize_fast(size);
		nsize = round_pagesize_fast(new_size);

		g_assert(nsize <= osize);

		if (osize != nsize) {
			size_t delta = osize - nsize;
			size_t n = pagecount_fast(delta);
			void *q = ptr_add_offset(p, nsize);

			g_assert(n >= 1);

			/*
			 * Memory regions that are larger than our highest-order cache
			 * are allocated and freed as-is, and never broken into smaller
			 * pages.  The purpose is to avoid excessive virtual memory space
			 * fragmentation, leading at some point to the unability for the
			 * kernel to find a large consecutive virtual memory region.
			 */

			vmm_stats.shrinkings++;

			if (n <= VMM_CACHE_LINES) {
				size_t m = n;
				vmm_invalidate_pages(q, delta);
				page_cache_coalesce_pages(&q, &m);
				if (page_cache_insert_pages(q, m)) {
					vmm_stats.free_to_cache++;
					vmm_stats.free_to_cache_pages += n;
				}
			} else {
				free_pages(q, delta, TRUE);
				vmm_stats.free_to_system++;
				vmm_stats.free_to_system_pages += n;
			}

			if (user_mem) {
				vmm_stats.user_memory -= delta;
				vmm_stats.user_pages -= n;
				g_assert(size_is_non_negative(vmm_stats.user_pages));
				g_assert(size_is_non_negative(vmm_stats.user_memory));
				memusage_remove(vmm_stats.user_mem, delta);
			} else {
				vmm_stats.core_memory -= delta;
				vmm_stats.core_pages -= n;
				g_assert(size_is_non_negative(vmm_stats.core_pages));
				g_assert(size_is_non_negative(vmm_stats.core_memory));
				memusage_remove(vmm_stats.core_mem, delta);
			}
		}
	}
}

/**
 * Shrink allocated user space via vmm_alloc() down to specified size.
 *
 * Calling with a NULL pointer is OK when the size is 0.
 * Otherwise calling with a new_size of 0 actually performs a vmm_free().
 */
void
vmm_shrink(void *p, size_t size, size_t new_size)
{
	vmm_shrink_internal(p, size, new_size, TRUE);
}

/**
 * Shrink allocated core space via vmm_core_alloc() down to specified size.
 *
 * Calling with a NULL pointer is OK when the size is 0.
 * Otherwise calling with a new_size of 0 actually performs a vmm_core_free().
 */
void
vmm_core_shrink(void *p, size_t size, size_t new_size)
{
	vmm_shrink_internal(p, size, new_size, FALSE);
}

/**
 * Scans the page cache for old pages and releases them if they have a certain
 * minimum age. We don't want to cache pages forever because they might never
 * be reused. Further, the OS would page them out anyway because they are
 * considered dirty even though we don't care about the content anymore. If we
 * recycled such old pages, the penalty from paging them in is unlikely lower
 * than the mmap()/munmap() overhead.
 */
static gboolean
page_cache_timer(gpointer unused_udata)
{
	static size_t line;
	time_t now = tm_time();
	struct page_cache *pc;
	size_t i;
	size_t expired = 0;
	size_t old_regions = vmm_pmap()->count;

	(void) unused_udata;

	if G_UNLIKELY(VMM_CACHE_LINES == line)
		line = 0;

	pc = &page_cache[line];

	spinlock(&pc->lock);

	if (vmm_debugging(pc->current > 0 ? 4 : 8)) {
		s_debug("VMM scanning page cache #%zu (%zu item%s)",
			line, pc->current, 1 == pc->current ? "" : "s");
	}

	for (i = 0; i < pc->current; /* empty */) {
		time_delta_t d = delta_time(now, pc->info[i].stamp);

		/*
		 * To avoid undue fragmentation of the memory space, do not free
		 * a block that lies within an already identified region too soon.
		 * Wait longer, for it to be further coalesced hopefully.
		 */

		if (
			d >= VMM_CACHE_MAXLIFE ||
			(
				d >= VMM_CACHE_LIFE && !pmap_is_within_region(vmm_pmap(),
					pc->info[i].base, pc->chunksize)
			)
		) {
			vpc_free(pc, i);
			expired++;
			vmm_stats.cache_expired++;
			vmm_stats.cache_expired_pages += pagecount_fast(pc->chunksize);
		} else {
			i++;
		}
	}

	if G_UNLIKELY(expired > 0) {
		if (vmm_debugging(1)) {
			size_t regions = vmm_pmap()->count;
			s_debug("VMM expired %zu item%s (%zuKiB total) from "
				"page cache #%zu (%zu item%s remaining), "
				"process has %zu VM regions%s",
				expired, 1 == expired ? "" : "s",
				expired * pc->chunksize / 1024, line,
				pc->current, 1 == pc->current ? "" : "s", regions,
				old_regions < regions ? " (fragmented further)" : "");
		}
		if (vmm_debugging(5)) {
			vmm_dump_pmap();
		}
	}

	spinunlock(&pc->lock);

	line++;			/* For next time */
	return TRUE;	/* Keep scheduling */
}

/**
 * Get a protected region bearing a non-NULL address.
 *
 * This is used as the address of sentinel objects for which we can do
 * pointer comparisons but which we shall never try to access directly as
 * they are "fake" object addresses.
 *
 * @return	A page-sized and page-aligned chunk of memory which causes an
 *			exception to be raised if accessed.
 */
const void *
vmm_trap_page(void)
{
	static const void *trap_page;

	if (NULL == trap_page) {
		void *p = alloc_pages(kernel_pagesize, FALSE, NULL);
		g_assert(p);
		mprotect(p, kernel_pagesize, PROT_NONE);
		trap_page = p;

		/* The trap page is accounted as user memory */
		vmm_stats.user_memory += kernel_pagesize;
		vmm_stats.user_pages++;
		vmm_stats.user_blocks++;
		memusage_add(vmm_stats.user_mem, kernel_pagesize);
	}
	return trap_page;
}

/**
 * @return whether the virtual memory segment grows with increasing addresses.
 */
gboolean
vmm_grows_upwards(void)
{
	return kernel_mapaddr_increasing;
}

/**
 * Set VMM debug level.
 */
void
set_vmm_debug(guint32 level)
{
	vmm_debug = level;
}

/**
 * Called when memory allocator has been initialized and it is possible to
 * call routines that will allocate core memory and perform logging calls.
 */
void
vmm_malloc_inited(void)
{
	safe_to_log = TRUE;
#ifdef TRACK_VMM
	vmm_track_malloc_inited();
#endif
}

/**
 * Dump VMM statistics to specified logging agent.
 */
G_GNUC_COLD void
vmm_dump_stats_log(logagent_t *la, unsigned options)
{
	struct pmap *pm = vmm_pmap();
	size_t cached_pages = 0, mapped_pages = 0, native_pages = 0;
	size_t i;

#define DUMP(x)	log_info(la, "VMM %s = %s", #x,		\
	(options & DUMP_OPT_PRETTY) ?					\
		uint64_to_gstring(vmm_stats.x) : uint64_to_string(vmm_stats.x))

	DUMP(allocations);
	DUMP(allocations_zeroed);
	DUMP(freeings);
	DUMP(shrinkings);
	DUMP(mmaps);
	DUMP(munmaps);
	DUMP(hints_followed);
	DUMP(hints_ignored);
	DUMP(alloc_from_cache);
	DUMP(alloc_from_cache_pages);
	DUMP(alloc_direct_core);
	DUMP(alloc_direct_core_pages);
	DUMP(free_to_cache);
	DUMP(free_to_cache_pages);
	DUMP(free_to_system);
	DUMP(free_to_system_pages);
	DUMP(forced_freed);
	DUMP(forced_freed_pages);
	DUMP(cache_evictions);
	DUMP(cache_coalescing);
	DUMP(cache_line_coalescing);
	DUMP(cache_expired);
	DUMP(cache_expired_pages);
	DUMP(high_order_coalescing);
	DUMP(pmap_foreign_discards);
	DUMP(pmap_foreign_discarded_pages);
	DUMP(pmap_overruled);

#undef DUMP
#define DUMP(x) log_info(la, "VMM pmap_%s = %s", #x,	\
	(options & DUMP_OPT_PRETTY) ?						\
		size_t_to_gstring(pm->x) : size_t_to_string(pm->x))

	DUMP(count);
	DUMP(size);
	DUMP(pages);
	DUMP(generation);

#undef DUMP
#define DUMP(x)	log_info(la, "VMM %s = %s", #x,		\
	(options & DUMP_OPT_PRETTY) ?					\
		size_t_to_gstring(vmm_stats.x) : size_t_to_string(vmm_stats.x))

	DUMP(user_memory);
	DUMP(user_pages);
	DUMP(user_blocks);
	DUMP(core_memory);
	DUMP(core_pages);

#undef DUMP

	/*
	 * Compute amount of cached pages.
	 */

	for (i = 0; i < VMM_CACHE_LINES; i++) {
		struct page_cache *pc = &page_cache[i];

		/* Don't spinlock, it's OK to have dirty reads here */

		cached_pages += pc->current * pc->pages;
	}

	/*
	 * Compute the amount of known native / mapped pages.
	 */

	mutex_get(&pm->lock);

	for (i = 0; i < pm->count; i++) {
		struct vm_fragment *vmf = &pm->array[i];

		if (vmf_is_native(vmf)) {
			native_pages += pagecount_fast(vmf_size(vmf));
		} else if (vmf_is_mapped(vmf)) {
			mapped_pages += pagecount_fast(vmf_size(vmf));
		}
	}

	mutex_release(&pm->lock);

#define DUMP(v,x)	log_info(la, "VMM %s = %s", (v),	\
	(options & DUMP_OPT_PRETTY) ?						\
		size_t_to_gstring(x) : size_t_to_string(x))

	DUMP("cached_pages", cached_pages);
	DUMP("mapped_pages", mapped_pages);
	DUMP("native_pages", native_pages);

	/*
	 * "computed_native_pages" MUST be equal to "native_pages" or it means
	 * we're not accounting the allocated pages correctly, either in the
	 * statistics or in the pmap regions.
	 */

	DUMP("computed_native_pages",
		cached_pages + vmm_stats.user_pages + vmm_stats.core_pages +
		local_pmap.pages + kernel_pmap.pages);

#undef DUMP
}

/**
 * Dump VMM statistics at exit time, along with the current pmap.
 */
G_GNUC_COLD void
vmm_dump_stats(void)
{
	s_info("VMM running statistics:");
	vmm_dump_stats_log(log_agent_stderr_get(), 0);
	vmm_dump_pmap();
}

/**
 * Dump VMM usage statistics to specified logging agent.
 */
G_GNUC_COLD void
vmm_dump_usage_log(logagent_t *la, unsigned options)
{
	if (NULL == vmm_stats.user_mem) {
		log_warning(la, "VMM user memory usage stats not configured");
	} else {
		memusage_summary_dump_log(vmm_stats.user_mem, la, options);
	}
	if (NULL == vmm_stats.core_mem) {
		log_warning(la, "VMM core memory usage stats not configured");
	} else {
		memusage_summary_dump_log(vmm_stats.core_mem, la, options);
	}
}

/**
 * In case an assertion failure occurs in this file, dump statistics
 * and the pmap.
 */
static G_GNUC_COLD void
vmm_crash_hook(void)
{
	int dummy;

	s_debug("VMM pagesize=%zu bytes, virtual addresses are %s",
		kernel_pagesize,
		kernel_mapaddr_increasing ? "increasing" : "decreasing");

	s_debug("VMM base=%p, initial_sp=%p, current_sp=%p (stack growing %s)",
		vmm_base, initial_sp, (void *) &dummy,
		ptr_cmp(initial_sp, &dummy) < 0 ? "up" : "down");

	vmm_dump_stats();
}

/**
 * Mark "amount" bytes as foreign in the local pmap, reserved for the stack.
 *
 * When the kernel pmap is loaded, simply make sure we find the stack
 * region and reserve an extra VMM_STACK_MINSIZE bytes for it to grow
 * further.
 */
static G_GNUC_COLD void
vmm_reserve_stack(size_t amount)
{
	const void *stack_base, *stack_end, *stack_low;

	/*
	 * If we could read the kernel pmap, reserve an extra VMM_STACK_MINSIZE
	 * after the stack, as a precaution.
	 */

	if (vmm_pmap() == &kernel_pmap) {
		struct vm_fragment *vmf = pmap_lookup(&kernel_pmap, initial_sp, NULL);
		gboolean first_time = NULL == vmm_base;

		if (NULL == vmf) {
			if (vmm_debugging(0)) {
				s_warning("VMM no stack region found in the kernel pmap");
			}
			if (NULL == vmm_base)
				vmm_base = vmm_trap_page();
		} else {
			const void *reserve_start;

			if (vmm_debugging(1)) {
				s_debug("VMM stack region found in the kernel pmap (%zu KiB)",
					vmf_size(vmf) / 1024);
			}

			stack_end = deconstify_gpointer(
				sp_increasing ? vmf->end : vmf->start);
			reserve_start = const_ptr_add_offset(stack_end,
				(kernel_mapaddr_increasing ? +1 : -1) * VMM_STACK_MINSIZE);

			if (
				!pmap_is_available(&kernel_pmap,
					reserve_start, VMM_STACK_MINSIZE)
			) {
				if (vmm_debugging(0)) {
					s_warning("VMM cannot reserve extra %uKiB %s stack",
						VMM_STACK_MINSIZE / 1024,
						sp_increasing ? "after" : "before");
				}
			} else {
				pmap_insert_foreign(&kernel_pmap,
					reserve_start, VMM_STACK_MINSIZE);
				if (vmm_debugging(1)) {
					s_debug("VMM reserved [%p, %p] "
						"%s stack for possible growing",
						reserve_start,
						const_ptr_add_offset(reserve_start,
							VMM_STACK_MINSIZE - 1),
						sp_increasing ? "after" : "before");
					vmm_dump_pmap();
				}
			}

			if (NULL == vmm_base)
				vmm_base = reserve_start;
		}
		if (first_time)
			goto vm_setup;
		return;
	}

	g_assert(amount != 0);

	/*
	 * If stack and VM region grow in opposite directions, there is ample
	 * room for the stack provided their relative position is correct.
	 */

	if ((size_t) -1 == amount) {
		vmm_base = vmm_trap_page();
		goto vm_setup;
	}

	stack_base = page_start(initial_sp);
	if (!sp_increasing)
		stack_base = const_ptr_add_offset(stack_base, kernel_pagesize);

	stack_end = const_ptr_add_offset(stack_base,
		(sp_increasing ? +1 : -1) * amount);
	stack_low = sp_increasing ? stack_base : stack_end;

	if (pmap_is_available(&local_pmap, stack_low, amount)) {
		pmap_insert_foreign(&local_pmap, stack_low, amount);
		if (vmm_debugging(1)) {
			s_debug("VMM reserved %zuKiB [%p, %p] for the stack",
				amount / 1024, stack_low,
				const_ptr_add_offset(stack_low, amount - 1));
		}
		if (kernel_mapaddr_increasing) {
			const void *after_stack = const_ptr_add_offset(stack_low, amount);
			vmm_base = vmm_trap_page();
			if (ptr_cmp(vmm_base, after_stack) > 0)
				vmm_base = after_stack;
		} else {
			vmm_base = vmm_trap_page();
			if (ptr_cmp(vmm_base, stack_low) < 0)
				vmm_base = stack_low;
		}
	} else {
		if (vmm_debugging(0)) {
			s_warning("VMM cannot reserve %zuKiB [%p, %p] for the stack",
				amount / 1024, stack_low,
				const_ptr_add_offset(stack_low, amount - 1));
			vmm_dump_pmap();
		}
		vmm_base = vmm_trap_page();
	}

vm_setup:
	if (vmm_debugging(0)) {
		s_debug("VMM will allocate pages from %p %swards",
			vmm_base, kernel_mapaddr_increasing ? "up" : "down");
	}
}

/**
 * Enable memory usage statistics collection.
 */
G_GNUC_COLD void
vmm_memusage_init(void)
{
	g_assert(NULL == vmm_stats.user_mem);
	g_assert(NULL == vmm_stats.core_mem);

	vmm_stats.user_mem = memusage_alloc("VMM user", 0);
	vmm_stats.core_mem = memusage_alloc("VMM core", 0);
}

/**
 * Called later in the initialization chain once the callout queue has been
 * initialized and the properties loaded.
 */
G_GNUC_COLD void
vmm_post_init(void)
{
	struct {
		unsigned non_default:1;
		unsigned vmm_invalidate_free_pages:1;
		unsigned vmm_protect_free_pages:1;
	} settings = { FALSE, FALSE, FALSE };

	crash_hook_add(_WHERE_, vmm_crash_hook);

	/*
	 * Log VMM configuration.
	 */

#ifdef VMM_INVALIDATE_FREE_PAGES
	settings.non_default = TRUE;
	settings.vmm_invalidate_free_pages = TRUE;
#endif
#ifdef VMM_PROTECT_FREE_PAGES
	settings.non_default = TRUE;
	settings.vmm_protect_free_pages = TRUE;
#endif

	if (settings.non_default) {
		s_message("VMM settings: %s%s",
			settings.vmm_invalidate_free_pages ?
				"VMM_INVALIDATE_FREE_PAGES" : "",
			settings.vmm_protect_free_pages ?
				"VMM_PROTECT_FREE_PAGES" : "");
	}

	if (vmm_debugging(0)) {
		s_debug("VMM using %zu bytes for the page cache", sizeof page_cache);
		s_debug("VMM kernel grows virtual memory by %s addresses",
			kernel_mapaddr_increasing ? "increasing" : "decreasing");
		s_debug("VMM stack grows by %s addresses",
			sp_increasing ? "increasing" : "decreasing");
	}

	if (vmm_debugging(1)) {
#ifdef HAS_SBRK
		s_debug("VMM initial break at %p", initial_brk);
#endif
		s_debug("VMM stack bottom at %p", initial_sp);
	}

	pmap_load(&kernel_pmap);
	cq_periodic_main_add(1000, page_cache_timer, NULL);

	/*
	 * Check whether we have enough room for the stack to grow.
	 */

	{
		const void *vmbase = vmm_trap_page();	/* First page allocated */
		const void *end = const_ptr_add_offset(vmbase, kernel_pagesize);
		size_t room;

		if (ptr_cmp(initial_sp, vmbase) > 0) {
			if (!sp_increasing) {
				/*
				 * Stack is after the VM region and is decreasing, it can
				 * grow at most to the end of the allocated region.
				 */

				room = round_pagesize(ptr_diff(initial_sp, end));
			} else {
				/*
				 * Stack is after the VM region and is increasing.
				 * The kernel should be able to extend it.
				 */

				room = (size_t) -1;
			}
		} else {
			if (sp_increasing) {

				/*
				 * Stack is before the VM region and is increasing, it can
				 * grow at most to the start of the allocated region.
				 */

				room = round_pagesize(ptr_diff(vmbase, initial_sp));
			} else {
				/*
				 * Stack is before the VM region and is decreasing.
				 * The kernel should be able to extend it.
				 */

				room = (size_t) -1;
			}
		}

		if ((size_t) -1 == room) {
			if (vmm_debugging(0)) {
				s_debug("VMM kernel can grow the stack as needed");
			}
		} else if (room < VMM_STACK_MINSIZE) {
			s_warning("VMM stack has only %zuKiB to grow!", room / 1024);
		} else if (vmm_debugging(0)) {
			s_debug("VMM stack has at most %zuKiB to grow", room / 1024);
		}

		/*
		 * Reserve mappings for the stack so that we do not attempt
		 * to hint an allocation within that range.
		 */

		vmm_reserve_stack(room);
	}

#ifdef TRACK_VMM
	vmm_track_post_init();
#endif
}

/**
 * Early initialization of the vitual memory manager.
 *
 * @attention
 * No external memory allocation (malloc() and friends) can be done in this
 * routine, which is called very early at startup.
 */
G_GNUC_COLD void
vmm_init(const void *sp)
{
	static spinlock_t init_lck = SPINLOCK_INIT;
	int i;

	g_assert(sp != &i);

	/*
	 * Detect whether vmm_init() was already run due to an earlier vmm_alloc()
	 * call, which indicates that something went wrong already in the startup
	 * and the process had to allocate memory earlier than expected, probably
	 * due to error logging.
	 */

	spinlock(&init_lck);

	if G_UNLIKELY(0 != kernel_pagesize) {
		spinunlock(&init_lck);
		return;
	}

#ifdef HAS_SBRK
	initial_brk = sbrk(0);
#endif
	initial_sp = sp;
	sp_increasing = ptr_cmp(&i, sp) > 0;

	init_kernel_pagesize();

	for (i = 0; i < VMM_CACHE_LINES; i++) {
		struct page_cache *pc = &page_cache[i];
		size_t pages = i + 1;
		pc->pages = pages;
		pc->chunksize = pages * kernel_pagesize;
		spinlock_init(&pc->lock);
	}

	/*
	 * Allocate the pmaps.
	 */

	pmap_allocate(&local_pmap);
	pmap_allocate(&kernel_pmap);

	g_assert_log(2 == local_pmap.pages + kernel_pmap.pages,
		"local_pmap.pages = %zu, kernel_pmap.pages = %zu",
		local_pmap.pages, kernel_pmap.pages);

	/*
	 * Allocate the trap page early so that it is at the bottom of the
	 * memory space, hopefully (not really true on Linux, but close enough).
	 */
	(void) vmm_trap_page();

	/*
	 * The VMM trap page was allocated earlier, before we have the pmap
	 * structure to record it.
	 *
	 * Insert it now, as a native region since we have allocated it.
	 */

	pmap_insert(vmm_pmap(), vmm_trap_page(), kernel_pagesize);

	/*
	 * Determine how the kernel is growing the virtual memory region.
	 */
#ifdef MINGW32
	kernel_mapaddr_increasing = 1;
#else
	{
		void *p = alloc_pages(kernel_pagesize, FALSE, NULL);
		void *q = alloc_pages(kernel_pagesize, FALSE, NULL);

		kernel_mapaddr_increasing = ptr_cmp(q, p) > 0;

		free_pages(q, kernel_pagesize, FALSE);
		free_pages(p, kernel_pagesize, FALSE);
	}
#endif
#ifdef TRACK_VMM
	vmm_track_init();
#endif

	/*
	 * We can now use the VMM layer to allocate memory via xmalloc().
	 */

	spinunlock(&init_lck);
	xmalloc_vmm_inited();
}

/**
 * Signal that we're about to close down all activity.
 */
void
vmm_pre_close(void)
{
	/*
	 * It's still safe to log, however it's going to get messy with all
	 * the memory freeing activity.  Better avoid such clutter.
	 */

	safe_to_log = FALSE;		/* Turn logging off */
}

/**
 * Signal that we should stop freeing memory pages.
 *
 * This is mostly useful during final cleanup when halloc() replaces malloc()
 * because some parts of the cleanup code in glib or libc expect to be able
 * to access memory that has been freed and do not like us invalidating the
 * addresses.
 */
void
vmm_stop_freeing(void)
{
	memusage_free_null(&vmm_stats.user_mem);
	memusage_free_null(&vmm_stats.core_mem);

	stop_freeing = TRUE;

	if (vmm_debugging(0))
		s_debug("VMM will no longer release freed pages");
}

/**
 * Final shutdown.
 */
G_GNUC_COLD void
vmm_close(void)
{
	struct pmap *pm = vmm_pmap();
	size_t mapped_pages = 0;
	size_t mapped_memory = 0;
	size_t pages = 0;
	size_t native_pages = 0;
	size_t memory = 0;
	size_t i;

	/*
	 * Clear all cached pages.
	 */

	for (i = 0; i < VMM_CACHE_LINES; i++) {
		struct page_cache *pc = &page_cache[i];
		size_t j;

		spinlock(&pc->lock);

		for (j = 0; j < pc->current; j++) {
			vpc_free(pc, j);
		}

		spinunlock(&pc->lock);
	}

#ifdef TRACK_VMM
	vmm_track_close();
#endif

	/*
	 * Look at remaining regions (leaked pages?).
	 */

	for (i = 0; i < pm->count; i++) {
		struct vm_fragment *vmf = &pm->array[i];

		if (vmf_is_foreign(vmf))
			continue;

		/*
		 * Found an allocated region, still.
		 */

		if (vmf_is_native(vmf)) {
			size_t n = pagecount_fast(vmf_size(vmf));
			memory += vmf_size(vmf) / 1024;
			pages += n;
			native_pages += n;
		} else if (vmf_is_mapped(vmf)) {
			mapped_memory += vmf_size(vmf) / 1024;
			mapped_pages += pagecount_fast(vmf_size(vmf));
		} else {
			s_warning("VMM invalid memory fragment type (%d)", vmf->type);
		}
	}

	/*
	 * Substract "once" memory.
	 */

	{
		size_t opages = omalloc_page_count();
		size_t smem = stacktrace_memory_used();
		size_t spages = pagecount_fast(smem);
		size_t mmem = malloc_memory_used();
		size_t mpages = pagecount_fast(mmem);

		if (opages > pages) {
			s_warning("VMM omalloc() claims using %zu page%s, have %zu left",
				opages, 1 == opages ? "" : "s", pages);
		} else {
			pages -= opages;
			memory -= opages * (compat_pagesize() / 1024);
		}

		if (mpages > pages) {
			s_warning("VMM malloc() claims using %zu page%s, have %zu left",
				mpages, 1 == mpages ? "" : "s", pages);
		} else {
			pages -= mpages;
			memory -= mpages * (compat_pagesize() / 1024);
		}

		if (spages > pages) {
			s_warning("VMM stacktrace claims using %zu page%s, have %zu left",
				spages, 1 == spages ? "" : "s", pages);
		} else {
			pages -= spages;
			memory -= spages * (compat_pagesize() / 1024);
		}
	}

	if (pages != 0) {
		s_warning("VMM still holds %zu non-attributed page%s totaling %s KiB",
			pages, 1 == pages ? "" : "s", size_t_to_string(memory));
		if (vmm_stats.user_pages != 0) {
			s_warning("VMM holds %zu user page%s (%zu block%s) totaling %s KiB",
				vmm_stats.user_pages, 1 == vmm_stats.user_pages ? "" : "s",
				vmm_stats.user_blocks, 1 == vmm_stats.user_blocks ? "" : "s",
				size_t_to_string(vmm_stats.user_memory / 1024));
		}
		if (vmm_stats.core_pages != 0) {
			s_message("VMM holds %zu core page%s totaling %s KiB",
				vmm_stats.core_pages, 1 == vmm_stats.core_pages ? "" : "s",
				size_t_to_string(vmm_stats.core_memory / 1024));
		}
	}
	if (native_pages != vmm_stats.user_pages + vmm_stats.core_pages) {
		s_warning("VMM holds %zu native pages, but %zu user + %zu core = %zu",
			native_pages, vmm_stats.user_pages,
			vmm_stats.core_pages, vmm_stats.user_pages + vmm_stats.core_pages);
	}
	if (mapped_pages != 0) {
		s_warning("VMM still holds %zu memory-mapped page%s totaling %s KiB",
			mapped_pages, 1 == mapped_pages ? "" : "s",
			size_t_to_string(mapped_memory));
	}
}

/***
 *** mmap() and munmap() wrappers: the application should not call these
 *** system calls directly but go through the wrappers so that the VMM layer
 *** can keep an accurate view of the process's virtual memory map.
 ***/

/**
 * Wrapper of the mmap() system call.
 */
void *
vmm_mmap(void *addr, size_t length, int prot, int flags,
	int fd, fileoffset_t offset)
{
#ifdef HAS_MMAP
	void *p = mmap(addr, length, prot, flags, fd, offset);

	if G_LIKELY(p != MAP_FAILED) {
		size_t size = round_pagesize_fast(length);

		vmm_stats.mmaps++;

		/*
		 * The mapped memory region is "foreign" memory as far as we are
		 * concerned and may overlap with previously allocated "foreign"
		 * chunks in whole or in part.
		 *
		 * Invoke pmap_overrule() before pmap_insert_mapped() to make
		 * sure we clean up our memory map before attempting to insert
		 * a new chunk since the insertion code is not prepared to handle
		 * all the overlapping cases we can encounter.
		 */

		pmap_overrule(vmm_pmap(), p, size, VMF_MAPPED);
		pmap_insert_mapped(vmm_pmap(), p, size);
		assert_vmm_is_allocated(p, length, VMF_MAPPED);

		if (vmm_debugging(5)) {
			s_debug("VMM mapped %zuKiB region at %p (fd #%d, offset 0x%lx)",
				length / 1024, p, fd, (unsigned long) offset);
		}
	} else if (vmm_debugging(0)) {
		s_warning("VMM FAILED maping of %zuKiB region (fd #%d, offset 0x%lx)",
			length / 1024, fd, (unsigned long) offset);
	}

	return p;
#else	/* !HAS_MMAP */
	(void) addr;
	(void) length;
	(void) prot;
	(void) flags;
	(void) fd;
	(void) offset;
	g_assert_not_reached();

	return MAP_FAILED;
#endif	/* HAS_MMAP */
}

/**
 * Wrappper of the munmap() system call.
 */
int
vmm_munmap(void *addr, size_t length)
{
#if defined(HAS_MMAP)
	int ret;

	assert_vmm_is_allocated(addr, length, VMF_MAPPED);

	ret = munmap(addr, length);

	if G_LIKELY(0 == ret) {
		pmap_remove(vmm_pmap(), addr, round_pagesize_fast(length));
		vmm_stats.munmaps++;

		if (vmm_debugging(5)) {
			s_debug("VMM unmapped %zuKiB region at %p", length / 1024, addr);
		}
	} else {
		s_warning("munmap() failed: %m");
	}

	return ret;
#else	/* !HAS_MMAP */
	(void) addr;
	(void) length;
	g_assert_not_reached();

	return -1;
#endif	/* HAS_MMAP */
}

/***
 *** Allocation tracking -- enabled by compiling with -DTRACK_VMM.
 ***/

/* FIXME -- the TRACK_VMM code is not thread-safe yet -- RAM, 2011-12-29 */

#ifdef TRACK_VMM
/**
 * This table assotiates an allocated pointer with a page_track structure.
 */
static hash_table_t *tracked;		/**< Allocations tracked */
static hash_table_t *not_leaking;	/**< Known non-leaks */

/**
 * Describes an allocated group of pages.
 */
struct page_track {
	size_t size;					/**< Length of consecutive pages */
	const char *file;				/**< File where allocation was made */
	int line;						/**< Line number within file */
#ifdef MALLOC_FRAMES
	const struct stackatom *ast;	/**< Allocation stack trace (atom) */
#endif
#ifdef MALLOC_TIME
	time_t atime;					/**< Allocation time */
#endif
	unsigned user:1;				/**< User memory or core? */
};

/**
 * Because we're going to use hash_table_t to keep track of the allocated
 * pages, and due to the fact that this data structure internally relies on
 * the VMM layer, we may recurse to the tracking code at any time.
 *
 * This flag keeps track of recursions so that we avoid exercising any
 * tracking when recursion is detected.
 */
static gboolean vmm_recursed;

/**
 * This buffer allows us to queue allocations or deletions whenever a
 * tracking activity occurs and we've detected recursion.
 */
#define VMM_BUFFER	16

enum track_operation {
	VMM_ALLOC = 0,
	VMM_FREE
};

struct track_buffer {
	enum track_operation op;
	const void *addr;
	struct page_track pt;
#ifdef MALLOC_FRAMES
	struct stacktrace where;
#endif
};

struct buffering {
	size_t idx;				/**< Next available slot (0 = empty) */
	size_t missed;			/**< "could not buffer" events */
	size_t max;				/**< Max buffer index, for logging */
};

static struct track_buffer vmm_buffered[VMM_BUFFER];
static struct buffering vmm_buffer;

/**
 * Buffering of non-leaking events that happen before we're fully initialized
 */

static const void *vmm_nl_buffered[VMM_BUFFER];
static struct buffering vmm_nl_buffer;

/**
 * Human version of operations.
 */
static const char *
track_operation_to_string(enum track_operation op)
{
	switch (op) {
	case VMM_ALLOC:	return "alloc";
	case VMM_FREE:	return "free";
	}

	return "unknown operation";
}

static void unbuffer_operations(void);
static void vmm_free_record(const void *p, size_t size, gboolean user_mem,
	const char *file, int line);

/**
 * User or core memory?
 */
static const char *
track_mem(gboolean user_mem)
{
	return user_mem ? "user" : "core";
}

/**
 * Buffer operation for deferred processing (up to next alloc/free).
 * We're buffering the fact that the operation took place, not deferring the
 * actual allocation / free that is described.
 */
static void
buffer_operation(enum track_operation op,
	const void *p, size_t size, gboolean user_mem, const char *file, int line)
{
	if (vmm_buffer.idx >= G_N_ELEMENTS(vmm_buffered)) {
		vmm_buffer.missed++;
		if (vmm_debugging(0)) {
			s_warning("VMM unable to defer tracking of "
				"%s (%zu %s bytes starting %p) at \"%s:%d\" (issue #%zu)",
				track_operation_to_string(op),
				track_mem(user_mem), p, file, line, vmm_buffer.missed);
			stacktrace_where_print(stderr);
		}
	} else {
		size_t i = vmm_buffer.idx++;
		struct track_buffer *tb = &vmm_buffered[i];

		if (vmm_buffer.idx > vmm_buffer.max) {
			vmm_buffer.max = vmm_buffer.idx;
		}

		if (vmm_debugging(5)) {
			s_warning("VMM deferring tracking of "
				"%s (%zu %s bytes starting %p) at \"%s:%d\" (item #%zu)",
				track_operation_to_string(op),
				size, track_mem(user_mem), p, file, line, vmm_buffer.idx);
		}

		tb->op = op;
		tb->addr = p;
		tb->pt.size = size;
		tb->pt.file = file;
		tb->pt.line = line;
		tb->pt.user = booleanize(user_mem);
#ifdef MALLOC_FRAMES
		stacktrace_get_offset(&tb->where, 2);
#endif
#ifdef MALLOC_TIME
		tb->pt.atime = tm_time();
#endif
	}
}

/*
 * In case TRACK_MALLOC is activated, we want the raw xpmalloc() and xfree()
 * from here on...
 *
 * For that reason, the following code should stay at the bottom of the file.
 */
#undef xpmalloc
#undef xfree

/**
 * Handle freeing of page, as described by the supplied page_track structure.
 */
static void
vmm_free_record_desc(const void *p, const struct page_track *pt)
{
	struct page_track *xpt;

	g_assert(pt != NULL);

	xpt = hash_table_lookup(tracked, p);

	if (NULL == xpt) {
		/*
		 * If we're dealing with a "core" region, we can free any sub-part
		 * of the initially allocated region, not just the start of the
		 * region.
		 *
		 * Therefore, our tracking of "core" regions is imperfect and we
		 * must not emit any warning when the base address is not found.
		 */

		if (!pt->user)
			return;			/* Freeing middle of "core" region */

		if (vmm_debugging(0)) {
			s_carp("VMM (%s:%d) attempt to free %s page at %p twice?",
				pt->file, pt->line, track_mem(pt->user), p);
			if (0 == vmm_buffer.missed) {
				s_error_from(_WHERE_,
					"VMM vmm_free() of unknown address %p", p);
			}
		}
		return;
	}

	/*
	 * Again, since we can free any part of a core region before freeing
	 * the beginning, we may end up with a much smaller region to free when
	 * the base pointer (which is the only thing we track) is finally freed.
	 *
	 * Only warn for "user" regions.
	 */

	if (pt->user && xpt->size != pt->size) {
		if (vmm_debugging(0)) {
			s_carp("VMM (%s:%d) freeing %s page at %p (%zu bytes) "
				"from \"%s:%d\" with wrong size %zu [%zu missed event%s]",
				pt->file, pt->line, track_mem(xpt->user), p,
				xpt->size, xpt->file, xpt->line, pt->size,
				vmm_buffer.missed, 1 == vmm_buffer.missed ? "" : "s");
		}
	}

	if (xpt->user != pt->user) {
		if (vmm_debugging(0)) {
			s_carp("VMM (%s:%d) freeing %s page at %p (%zu bytes) "
				"from \"%s:%d\" as wrong type \"%s\" [%zu missed event%s]",
				pt->file, pt->line, track_mem(xpt->user), p,
				xpt->size, xpt->file, xpt->line, track_mem(pt->user),
				vmm_buffer.missed, 1 == vmm_buffer.missed ? "" : "s");
		}
	}

	hash_table_remove(tracked, p);
	xfree(xpt);						/* raw free() */
}

/**
 * Handle allocation as described in the supplied page_track structure.
 */
static void
vmm_alloc_record_desc(const void *p, const struct page_track *pt)
{
	struct page_track *xpt;

	g_assert(pt != NULL);

	if (NULL != (xpt = hash_table_lookup(tracked, p))) {
		s_warning("VMM (%s:%d) reusing page start %p (%zu %s bytes) "
			"from %s:%d, missed its freeing",
			pt->file, pt->line, p, xpt->size,
			track_mem(xpt->user), xpt->file, xpt->line);
#ifdef MALLOC_FRAMES
		s_warning("VMM %s page %p was allocated from:",
			track_mem(xpt->user), p);
		stacktrace_atom_print(stderr, xpt->ast);
#endif
		s_warning("VMM current stack:");
		stacktrace_where_print(stderr);

		vmm_free_record_desc(p, pt);
	}

	xpt = xpmalloc(sizeof *xpt);	/* raw malloc() */
	*xpt = *pt;						/* struct copy */

	if (!hash_table_insert(tracked, p, xpt))
		s_error("cannot record page allocation");
}

/**
 * Record that ``p'' is ``size'' bytes long and was allocated from "file:line".
 *
 * @return its argument ``p''
 */
static void *
vmm_alloc_record(void *p, size_t size, gboolean user_mem,
	const char *file, int line)
{
	struct page_track pt;

	if (NULL == tracked)
		return p;					/* Tracking not initialized yet */

	if (vmm_recursed) {
		buffer_operation(VMM_ALLOC, p, size, user_mem, file, line);
		return p;
	}

	if (vmm_buffer.idx != 0) {
		unbuffer_operations();
	}

	/*
	 * Critical section protected against recursion.
	 */

	g_assert(!vmm_recursed);

	vmm_recursed = TRUE;
	pt.size = size;
	pt.file = file;
	pt.line = line;
	pt.user = booleanize(user_mem);
#ifdef MALLOC_FRAMES
	{
		struct stacktrace st;

		stacktrace_get_offset(&st, 1);
		pt.ast = stacktrace_get_atom(&st);
	}
#endif
#ifdef MALLOC_TIME
	pt.atime = tm_time();
#endif

	vmm_alloc_record_desc(p, &pt);
	vmm_recursed = FALSE;

	return p;
}

/**
 * Record that ``p'' (pointing to ``size'' bytes is now free.
 */
static void
vmm_free_record(const void *p, size_t size, gboolean user_mem,
	const char *file, int line)
{
	struct page_track pt;

	if (vmm_recursed) {
		buffer_operation(VMM_FREE, p, size, user_mem, file, line);
		return;
	}

	if (vmm_buffer.idx != 0) {
		unbuffer_operations();
	}

	/*
	 * Critical section protected against recursion.
	 */

	g_assert(!vmm_recursed);

	vmm_recursed = TRUE;
	pt.size = size;
	pt.file = file;
	pt.line = line;
	pt.user = booleanize(user_mem);
	vmm_free_record_desc(p, &pt);
	vmm_recursed = FALSE;
}

/**
 * Shift out the first entry in the buffer, copying the value into the
 * supplied structure.
 */
static void
unbuffer_first(struct track_buffer *dest)
{
	g_assert(dest != NULL);
	g_assert(size_is_positive(vmm_buffer.idx));
	g_assert(vmm_buffer.idx <= G_N_ELEMENTS(vmm_buffered));

	*dest = vmm_buffered[0];		/* Struct copy */

	/* Shift everything down one position */
	memmove(&vmm_buffered[0], &vmm_buffered[1],
		(vmm_buffer.idx - 1) * sizeof vmm_buffered[0]);

	vmm_buffer.idx--;			/* One more slot available */
}

/**
 * Unbuffer operations in the order they were issued.
 */
static void unbuffer_operations(void)
{
	g_assert(size_is_positive(vmm_buffer.idx));

	while (vmm_buffer.idx != 0) {
		struct track_buffer tb;

		/*
		 * Shift the oldest operation before attempting to pocess it,
		 * making at least space for one more bufferable operation.
		 */

		unbuffer_first(&tb);		/* Decreases vmm_buffer.idx */

		g_assert(!vmm_recursed);
		vmm_recursed = TRUE;

		/*
		 * Process buffered operation.
		 */

		if (vmm_debugging(2)) {
			s_warning("VMM processing deferred "
				"%s (%zu %s bytes starting %p) at \"%s:%d\" "
				"(%zu other record%s pending)",
				track_operation_to_string(tb.op), tb.pt.size,
				track_mem(tb.pt.user), tb.addr, tb.pt.file, tb.pt.line,
				vmm_buffer.idx, 1 == vmm_buffer.idx ? "" : "s");
		}

		switch (tb.op) {
		case VMM_ALLOC:
#ifdef MALLOC_FRAMES
			tb.pt.ast = stacktrace_get_atom(&tb.where);
#endif
			vmm_alloc_record_desc(tb.addr, &tb.pt);
			break;
		case VMM_FREE:
#ifdef MALLOC_FRAMES
			tb.pt.ast = NULL;
#endif
			vmm_free_record_desc(tb.addr, &tb.pt);
			break;
		}

		vmm_recursed = FALSE;
	}
}

/**
 * Record a non-leaking address.
 */
static void *
vmm_not_leaking(const void *o)
{
	if (not_leaking != NULL) {
		hash_table_insert(not_leaking, o, GINT_TO_POINTER(1));
	} else {
		/*
		 * Buffer the event until we're initialized.
		 */

		if (vmm_nl_buffer.idx >= G_N_ELEMENTS(vmm_nl_buffered)) {
			vmm_nl_buffer.missed++;
		} else {
			size_t i = vmm_nl_buffer.idx++;

			if (vmm_nl_buffer.idx > vmm_nl_buffer.max) {
				vmm_nl_buffer.max = vmm_nl_buffer.idx;
			}
			vmm_nl_buffered[i] = o;
		}
	}

	return deconstify_gpointer(o);
}

/**
 * Tracking version of vmm_alloc().
 */
void *
vmm_alloc_track(size_t size, gboolean user_mem, const char *file, int line)
{
	void *p;

	p = vmm_alloc_internal(size, user_mem);
	return vmm_alloc_record(p, size, user_mem, file, line);
}

/**
 * Tracking version of vmm_alloc() for memory flagged as non-leaking.
 */
void *
vmm_alloc_track_not_leaking(size_t size, const char *file, int line)
{
	void *p;

	p = vmm_alloc_track(size, TRUE, file, line);
	return vmm_not_leaking(p);
}

/**
 * Tracking version of vmm_alloc0().
 */
void *
vmm_alloc0_track(size_t size, const char *file, int line)
{
	void *p;

	p = vmm_alloc0(size);
	return vmm_alloc_record(p, size, TRUE, file, line);
}

/**
 * Tracking version of vmm_free().
 */
void
vmm_free_track(void *p, size_t size, gboolean user_mem,
	const char *file, int line)
{
	if (not_leaking != NULL) {
		hash_table_remove(not_leaking, p);
	}

	vmm_free_record(p, size, user_mem, file, line);
	vmm_free_internal(p, size, user_mem);
}

/**
 * Tracking version of vmm_shrink().
 */
void
vmm_shrink_track(void *p, size_t size, size_t new_size, gboolean user_mem,
	const char *file, int line)
{
	/* The shrinking point becomes the new allocation point (file, line) */
	vmm_free_record(p, size, user_mem, file, line);
	vmm_alloc_record(p, new_size, user_mem, file, line);
	vmm_shrink_internal(p, size, new_size, user_mem);
}

/**
 * Initialization of the VMM tracking.
 */
static void
vmm_track_init(void)
{
	tracked = hash_table_new_real();
	not_leaking = hash_table_new_real();

	/*
	 * Handle buffered non-leaking events.
	 */

	if (vmm_nl_buffer.idx != 0) {
		size_t i;
		for (i = 0; i < vmm_nl_buffer.idx; i++) {
			vmm_not_leaking(vmm_nl_buffered[i]);
		}
	}
}

/**
 * Malloc was initialized, we can log.
 * Display warning if we overflowed our buffering.
 */
static void
vmm_track_malloc_inited(void)
{
	if (vmm_buffer.missed != 0) {
		s_warning("VMM missed %zu initial tracking event%s",
			vmm_buffer.missed, 1 == vmm_buffer.missed ? "" : "s");
	}
}

/**
 * Called when properties have been loaded, so we can use vmm_debugging().
 */
static void
vmm_track_post_init(void)
{
	if (vmm_buffer.max > 0 && vmm_debugging(0)) {
		s_debug("VMM required %zu bufferred event%s",
			vmm_buffer.max, 1 == vmm_buffer.max ? "" : "s");
	}
	if (vmm_nl_buffer.max > 0 && vmm_debugging(0)) {
		s_debug("VMM required %zu bufferred non-leaking event%s",
			vmm_nl_buffer.max, 1 == vmm_nl_buffer.max ? "" : "s");
	}
}

/**
 * vmm_log_pages		-- hash table iterator callback
 *
 * Log used pages, and record them among the `leaksort' set for future summary.
 */
static void
vmm_log_pages(const void *k, void *v, gpointer leaksort)
{
	const struct page_track *pt = v;
	char ago[32];

	if (hash_table_lookup(not_leaking, k))
		return;

	/*
	 * If page is a "core" page, then it was allocated by a memory allocator
	 * for its own supply of memory to manage and distribute to users, not
	 * for direct "user" consumption.  Therefore, we're not interested by
	 * its leaking.
	 */

	if (!pt->user)
		return;

#ifdef MALLOC_TIME
	gm_snprintf(ago, sizeof ago, " [%s]",
		short_time(delta_time(tm_time(), pt->atime)));
#else
	ago[0] = '\0';
#endif	/* MALLOC_TIME */

	s_warning("leaked %s page%s %p (%zu bytes) from \"%s:%d\"%s",
		track_mem(pt->user), pt->size > kernel_pagesize ? "s" : "",
		k, pt->size, pt->file, pt->line, ago);

	leak_add(leaksort, pt->size, pt->file, pt->line);

#ifdef MALLOC_FRAMES
	s_message("%s block %p allocated from: ", track_mem(pt->user), k);
	stacktrace_atom_print(stderr, pt->ast);
#endif
}

/**
 * Report leaks.
 */
static void
vmm_track_close(void)
{
	void *leaksort;

	leaksort = leak_init();
	hash_table_foreach(tracked, vmm_log_pages, leaksort);
	leak_dump(leaksort);
	leak_close(leaksort);

	/*
	 * Do not destroy ``tracked'', in case the final messages we emit cause
	 * a memory allocation and the VMM layer is called again.
	 */
}

/**
 * The raw vmm_alloc().
 */
void *
vmm_alloc_notrack(size_t size)
{
	return vmm_alloc(size);
}

/**
 * The raw vmm_free().
 */
void
vmm_free_notrack(void *p, size_t size)
{
	vmm_free(p, size);
}
#endif	/* TRACK_VMM */

/* vi: set ts=4 sw=4 cindent: */
