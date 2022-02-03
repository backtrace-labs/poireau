#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cc.h"

/* SPDX-License-Identifier: MIT */

/**
 * Attempt to sample roughly every 32 MB of allocated bytes by
 * default.
 */
#define DEFAULT_SAMPLE_PERIOD (1UL << 25)

/**
 * Fetch the runtime-defined allocation sample period from this
 * environment variable.
 */
#define SAMPLE_PERIOD_ENV_VAR "POIREAU_SAMPLE_PERIOD_BYTES"

/**
 * Each thread should have a zero-initialised sample state struct.
 */
struct sample_state {
	uint64_t s[4];
	size_t bytes_until_next_sample;
};

/**
 * Determines whether this allocation request should be sampled.
 */
static inline bool sample_request(struct sample_state *, size_t);

/**
 * Should be called after `sample_request` returns true to update the
 * sample state.
 *
 * Returns whether we should immediately try to re-sample the request.
 */
NOINLINE bool sample_request_reset(struct sample_state *);

/**
 * Returns a pseudorandom value from U(0, 1].
 *
 * If the state was zero-filled, newly_initialized is set to true.
 *
 * Exposed only for testing.
 */
double sample_uniform(struct sample_state *, bool *newly_initialized);

static inline bool
sample_request(struct sample_state *state, size_t request)
{
	bool ret;

#if defined(__GCC_ASM_FLAG_OUTPUTS__) && defined(__x86_64__)
	/*
	 * Subtract the request from bytes_until_next_sample.  Sample
	 * if the result borrowed or is zero: that means
	 * request >= bytes_until_next_sample.
	 *
	 * We can't use asm goto because we have an output in
	 * state->bytes_until_next_sample.
	 */
	asm ("subq %[request], %[remaining]\n\t"
	     : [remaining] "+m"(state->bytes_until_next_sample),
	       "=@ccbe"(ret)
	     : [request] "r"(request)
	     : "cc");
#else
	uint64_t current = state->bytes_until_next_sample;
	uint64_t remaining = current - request;

	state->bytes_until_next_sample = remaining;
	ret = (request >= current);
#endif

	return ret;
}
