#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cc.h"

/**
 * We need an allocation tracking subsystem to trigger a tracepoint
 * when we free an object that was traced at allocation time.
 *
 * Allocating a tracked object is a rare event, so the code is
 * designed to make it fast to determine that an object is *not*
 * tracked.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Up this if we ever opt into 5-level page tables.
 */
#define ADDRESS_SPACE_MAX (1UL << 47)

/*
 * We align every tracked allocation to 1GB.  Any misaligned
 * allocation is definitely not tracked; an aligned allocation may or
 * may not be tracked.
 */
#define TRACKING_ALIGNMENT (1UL << 30)

struct tracked_alloc_info {
	uint64_t id;
	size_t size;
};

/**
 * Each tracked allocation maps to an entry in this array with a
 * simple division by the TRACKING_ALIGNMENT.  Each entry in this
 * table is equal to the allocated address.
 *
 * A prospective (aligned) allocation is tracked iff its corresponding
 * entry in the table has the same address.
 */
extern uintptr_t tracked_alloc_table[ADDRESS_SPACE_MAX / TRACKING_ALIGNMENT];

static inline bool
tracked_alloc_p(const void *ptr)
{
	uintptr_t bits = (uintptr_t)ptr;

	/* Skip even reading the table in the common case. */
	if (LIKELY((bits % TRACKING_ALIGNMENT) != 0))
		return false;

	if (ptr == NULL)
		return false;

	return bits == tracked_alloc_table[bits / TRACKING_ALIGNMENT];
}

/**
 * Returns a fresh allocation of size `request`.  The region is always
 * zero-filled.
 */
void *tracked_alloc_get(size_t request, uint64_t *OUT_id);

struct tracked_alloc_info tracked_alloc_info(const void *);

void tracked_alloc_put(void *);
