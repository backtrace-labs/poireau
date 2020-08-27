#include "tracked_alloc.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/sdt.h>

/* SPDX-License-Identifier: MIT */

#define PAGE_SIZE 4096ULL

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static uint64_t alloc_id_counter = 1;

/**
 * We multiply alloc_ids by the multiplier constant, modulo the
 * location_mod to provide a "hint" location to mmap.  The hint is
 * semantically a no-op, so does not affect correctness.  We use it to
 * heuristically make sure we reuse addresses rarely, making it easier
 * to detect use-after-free.
 */
static const uintptr_t mmap_location_mod = 1UL << 47;
static const uintptr_t mmap_location_multiplier = 17ULL << 30;

/**
 * We use parallel arrays to let the fast path (tracked_alloc_p) use
 * simple addressing.
 */
uintptr_t tracked_alloc_table[ADDRESS_SPACE_MAX / TRACKING_ALIGNMENT];
static struct tracked_alloc_info info_table[ADDRESS_SPACE_MAX / TRACKING_ALIGNMENT];

/*
 * We rely on the kernel, via mmap, for mutual exclusion in the tables above.
 */

static void *
mmap_hint(uint64_t id)
{
	uintptr_t ret = (id * mmap_location_multiplier) % mmap_location_mod;

	return (void *)(ret & -PAGE_SIZE);
}
/**
 * Returns a fresh mapping of `size` bytes (rounded up to a page size),
 * aligned to `alignment`, which must be a power of two.
 *
 * Returns NULL on failure;
 */
static void *
aligned_mmap(uint64_t id, size_t size, size_t alignment)
{
	size_t rounded_size = (size + PAGE_SIZE - 1) & -PAGE_SIZE;
	size_t padded_size = rounded_size + alignment;
	uintptr_t map_begin, chunk_begin, chunk_end, map_end;
	void *map;
	int r;

	assert((alignment & (alignment - 1)) == 0);

	if (size > SSIZE_MAX)
		return NULL;

	map = mmap(mmap_hint(id), padded_size,
		  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		  -1, 0);
	if (map == MAP_FAILED) {
		DTRACE_PROBE4(libpoireau, mmap_failed, size, alignment,
		    padded_size, errno);
		return NULL;
	}

	map_begin = (uintptr_t)map;
	map_end = map_begin + padded_size;

	chunk_begin = (map_begin + alignment) & -alignment;
	if (chunk_begin != map_begin) {
		r = munmap((void *)map_begin, chunk_begin - map_begin);
		assert(r == 0 && "Header slop munmap failed.");
	}

	chunk_end = chunk_begin + rounded_size;
	if (chunk_end != map_end) {
		r = munmap((void *)chunk_end, map_end - chunk_end);
		assert(r == 0 && "Trailer slop munmap failed.");
	}

	return (void *)chunk_begin;
}

static void
aligned_munmap(void *ptr, size_t size, size_t alignment)
{
	uintptr_t bits = (uintptr_t)ptr;
	size_t rounded_size = (size + PAGE_SIZE - 1) & -PAGE_SIZE;
	int r;

	assert((bits & (alignment - 1)) == 0 &&
	    "Target munmap address must be aligned");
	if (ptr == NULL)
		return;

	r = munmap(ptr, rounded_size);
	assert(r == 0 && "Release munmap failed.");
	return;
}

void *
tracked_alloc_get(size_t request, uint64_t *OUT_id)
{
	void *alloc;
	uintptr_t prev;
	uint64_t id;
	size_t index;

	*OUT_id = 0;
	id = __atomic_fetch_add(&alloc_id_counter, 1, __ATOMIC_RELAXED);
	alloc = aligned_mmap(id, request, TRACKING_ALIGNMENT);
	if (alloc == NULL)
		return NULL;

	index = (uintptr_t)alloc / TRACKING_ALIGNMENT;
	__atomic_store_n(&info_table[index].id, id, __ATOMIC_RELEASE);
	__atomic_store_n(&info_table[index].size, request, __ATOMIC_RELEASE);
	prev = __atomic_exchange_n(&tracked_alloc_table[index], (uintptr_t)alloc,
	    __ATOMIC_ACQ_REL);
	assert(prev == 0 && "Heap corruption: double / invalid free.");

	*OUT_id = id;
	return alloc;
}

static void
shrink_mapping(void *ptr, size_t current, size_t desired)
{
	uintptr_t begin = (uintptr_t)ptr;
	uintptr_t end = (begin + current + PAGE_SIZE - 1) & -PAGE_SIZE;
	uintptr_t desired_end = (begin + desired + PAGE_SIZE - 1) & -PAGE_SIZE;
	int r;

	if (end == desired_end)
		return;

	r = munmap((void *)desired_end, end - desired_end);
	assert(r == 0 && "Shrink munmap failed.");
	return;
}

static bool
grow_mapping(void *ptr, size_t current, size_t desired)
{
	uintptr_t begin = (uintptr_t)ptr;
	uintptr_t end = (begin + current + PAGE_SIZE - 1) & -PAGE_SIZE;
	uintptr_t desired_end = (begin + desired + PAGE_SIZE - 1) & -PAGE_SIZE;
	void *ret;

	if (end == desired_end)
		return true;

	/*
	 * Try to grow the current mapping in place.
	 *
	 * MAP_FIXED_NOREPLACE asks the kernel to fail if there isn't
	 * enough empty space at `end`: unlike MAP_FIXED, existing
	 * mappings are left as is.
	 *
	 * If the kernel does not implement MAP_FIXED_NOREPLACE, it
	 * may instead hand us a different address; we will immediately
	 * remove the mapping and return with failure.
	 *
	 * With or without MAP_FIXED_NOREPLACE, we only succeed if the
	 * new mapping immediately follows the current one, and
	 * otherwise do not leave any new mapping around.
	 */
	ret = mmap((void *)end, desired_end - end,
		  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED_NOREPLACE,
		  -1, 0);

	if (ret == MAP_FAILED)
		return false;

	if (ret != (void *)end) {
		int r;

		r = munmap(ret, desired_end - end);
		assert(r == 0 && "Cleanup growth munmap failed.");
		return false;
	}

	return true;
}

bool
tracked_alloc_resize(void *ptr, size_t request)
{
	struct tracked_alloc_info info;
	size_t index = (uintptr_t)ptr / TRACKING_ALIGNMENT;
	bool r;

	info = tracked_alloc_info(ptr);
	if (request == info.size)
		return true;

	if (request < info.size) {
		shrink_mapping(ptr, info.size, request);
		r = true;
	} else {
		r = grow_mapping(ptr, info.size, request);
	}

	if (r == true)
		__atomic_store_n(&info_table[index].size, request,
		    __ATOMIC_RELEASE);

	return r;
}

struct tracked_alloc_info
tracked_alloc_info(const void *ptr)
{
	uintptr_t expected;
	size_t index = (uintptr_t)ptr / TRACKING_ALIGNMENT;

	expected = __atomic_load_n(&tracked_alloc_table[index], __ATOMIC_ACQUIRE);
	assert(expected == (uintptr_t)ptr && "Heap corruption double / invalid free.");

	return (struct tracked_alloc_info) {
		.id = __atomic_load_n(&info_table[index].id, __ATOMIC_ACQUIRE),
		.size = __atomic_load_n(&info_table[index].size, __ATOMIC_ACQUIRE),
	};
}

void
tracked_alloc_put(void *ptr)
{
	struct tracked_alloc_info info;
	uintptr_t prev;
	size_t index = (uintptr_t)ptr / TRACKING_ALIGNMENT;

	prev = __atomic_load_n(&tracked_alloc_table[index], __ATOMIC_ACQUIRE);
	info = tracked_alloc_info(ptr);

	assert(prev == (uintptr_t)ptr && "Heap corruption double / invalid free.");
	assert(info.id != 0 && "Heap corruption: double / invalid free.");

	__atomic_store_n(&info_table[index].id, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&info_table[index].size, 0, __ATOMIC_RELEASE);
	prev = __atomic_exchange_n(&tracked_alloc_table[index], 0, __ATOMIC_ACQ_REL);
	assert(prev == (uintptr_t)ptr && "Heap corruption double / invalid free.");

	/*
	 * mmap/munmap ensure mutual exclusion. We mmap before
	 * publishing to the tables, and munmap after clearing the
	 * tables.
	 */
	aligned_munmap(ptr, info.size, TRACKING_ALIGNMENT);
	return;
}
