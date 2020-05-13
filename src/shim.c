#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/sdt.h>

#include "cc.h"
#include "sample.h"

static __thread struct sample_state sample_state;

/* SPDX-License-Identifier: MIT */

/*
 * The order is important: we want to lookup free and realloc first,
 * to make sure we never pass one of our own allocations to the
 * underlying malloc.
 */
#define FOREACH_WRAPPED(G)			\
	G(free)					\
	G(realloc)				\
	G(malloc)				\
	G(calloc)

/**
 * glibc dlsym allocates memory, but has a fallback code path when
 * allocation fails.
 *
 * Make sure to redirect calls to these dummy implementations while
 * we use dlsym / RTLD_NEXT.
 */
#define REDIRECT(F)						\
	static __typeof__(F) dummy_##F;				\
	static __typeof__(F) *volatile base_##F = dummy_##F;	\

FOREACH_WRAPPED(REDIRECT);
#undef REDIRECT

/*
 * Returns true if the pointers were all successfully patched.
 */
static COLD NOINLINE bool
init_shim(void)
{
	/*
	 * All the setup code is idempotent, so multiple
	 * initialisation is fine, as long as we don't get stuck in an
	 * infinite loop.
	 */
	static volatile uint8_t done = 0;
	static __thread uint8_t started = 0;

	if (done | started)
		return done != 0;

	started = 1;
#define LOOKUP(F) base_##F = dlsym(RTLD_NEXT, #F);
	FOREACH_WRAPPED(LOOKUP)
#undef LOOKUP

	done = 1;
	return true;
}

static COLD void
dummy_free(void *ptr)
{

	/*
	 * Once the pointers have all been initialised, we can
	 * tail-call into our own free(): base_free now points to the
	 * real underlying free implementation.
	 */
	if (init_shim())
		free(ptr);
	return;
}

static COLD void *
dummy_realloc(void *ptr, size_t request)
{

	if (init_shim())
		return realloc(ptr, request);
	return NULL;
}

static COLD void *
dummy_malloc(size_t request)
{

	if (init_shim())
		return malloc(request);
	return NULL;
}

static COLD void *
dummy_calloc(size_t num, size_t size)
{

	if (init_shim())
		return calloc(num, size);
	return NULL;
}

EXPORT HOT NOINLINE void
free(void *ptr)
{

	base_free(ptr);
	return;
}

static COLD NOINLINE void *
sampled_realloc(void *ptr, size_t request)
{

	if (sample_request_reset(&sample_state))
		return realloc(ptr, request);

	DTRACE_PROBE2(libpoireau, realloc, ptr, request);
	return base_realloc(ptr, request);
}

EXPORT HOT NOINLINE void *
realloc(void *ptr, size_t request)
{

	if (UNLIKELY(sample_request(&sample_state, request)))
		return sampled_realloc(ptr, request);

	return base_realloc(ptr, request);
}

static COLD NOINLINE void *
sampled_malloc(size_t request)
{

	if (sample_request_reset(&sample_state))
		return malloc(request);

	DTRACE_PROBE1(libpoireau, malloc, request);
	return base_malloc(request);
}

EXPORT HOT NOINLINE void *
malloc(size_t request)
{

	if (UNLIKELY(sample_request(&sample_state, request)))
		return sampled_malloc(request);

	return base_malloc(request);
}

static COLD NOINLINE void *
sampled_calloc(size_t num, size_t size)
{
	size_t req;

	if (sample_request_reset(&sample_state))
		return calloc(num, size);

	if (__builtin_umull_overflow(num, size, &req)) {
		DTRACE_PROBE2(libpoireau, calloc_overflow, num, size);
		return NULL;
	}

	DTRACE_PROBE2(libpoireau, calloc, num, size);
	return base_calloc(1, req);
}

EXPORT HOT NOINLINE void *
calloc(size_t num, size_t size)
{
	size_t req;

	if (UNLIKELY(__builtin_umull_overflow(num, size, &req) ||
	    sample_request(&sample_state, req)))
		return sampled_calloc(num, size);

	return base_calloc(1, req);
}
