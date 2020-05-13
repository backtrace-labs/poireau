#include "sample.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <sys/random.h>

#include "cc.h"

/* SPDX-License-Identifier: MIT AND CC0-1.0 */

/*
 * We use xoshiro256+ 1.0 to generate floating point uniform variates.
 *
 * Written in 2018 by David Blackman and Sebastiano Vigna (vigna@acm.org)
 *
 * To the extent possible under law, the author has dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide. This software is distributed without any warranty.
 *
 * See <http://creativecommons.org/publicdomain/zero/1.0/>.
 */
static inline uint64_t
rotl(const uint64_t x, int k)
{

	return (x << k) | (x >> (64 - k));
}

/*
 * We only return the top 52 bits: that's all we need, and low bits
 * are less uniformly distributed.
 */
static inline uint64_t
xoshiro_next(struct sample_state *state)
{
	static const size_t significand_bits = 52;
	static const size_t shift = 64 - significand_bits;
	uint64_t *s = state->s;
	const uint64_t result = s[0] + s[3];

	const uint64_t t = s[1] << 17;

	s[2] ^= s[0];
	s[3] ^= s[1];
	s[1] ^= s[2];
	s[0] ^= s[3];

	s[2] ^= t;

	s[3] = rotl(s[3], 45);

	return result >> shift;
}

/**
 * Returns whether the state was zero-filled and had to be initialized.
 */
static COLD bool
maybe_initialize_xoshiro(struct sample_state *state)
{
	int r;

	for (size_t i = 0; i < sizeof(state->s) / sizeof(state->s[0]); i++)
		if (state->s[i] != 0)
			return false;

	r = getentropy(state->s, sizeof(state->s));
	assert(r == 0 && "getentropy failed");
	return true;
}

static COLD NOINLINE uint64_t
sample_uniform_slow_path(struct sample_state *state, bool *newly_initialized)
{
	uint64_t ret;

	do {
		/*
		 * If the random state is all 0, we have to
		 * seed it.
		 */
		if (maybe_initialize_xoshiro(state))
			*newly_initialized = true;
		ret = xoshiro_next(state);
	} while (ret == 0);

	return ret;
}

double
sample_uniform(struct sample_state *state, bool *newly_initialized)
{
	uint64_t bits;
	union {
		double f;
		uint64_t bits;
	} u01 = {
		.f = 1.0,
	};

	bits = xoshiro_next(state);
	if (UNLIKELY(bits == 0))
		bits = sample_uniform_slow_path(state, newly_initialized);

	u01.bits |= bits;
	return u01.f - 1.0;
}

static double
sample_exponential(struct sample_state *state, double mean, bool *newly_initialized)
{

	return -mean * log(sample_uniform(state, newly_initialized));
}

bool
sample_request_reset(struct sample_state *state)
{

	do {
		bool newly_initialized = false;

		state->bytes_until_next_sample =
		    sample_exponential(state, sample_period, &newly_initialized);
		/*
		 * If we just initialised the state, we must test
		 * against the real threshold we just wrote in
		 * bytes_until_next_sample: otherwise, we'd bias by
		 * always sampling the first allocation in each
		 * thread.
		 */
		if (newly_initialized)
			return true;
	} while (UNLIKELY(state->bytes_until_next_sample == 0));

	return false;
}
