#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sdt.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "cc.h"
#include "sample.h"
#include "tracked_alloc.h"

#define PAGE_SIZE 4096ULL

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

static COLD NOINLINE void
sampled_free(void *ptr)
{
	struct tracked_alloc_info info;

	info = tracked_alloc_info(ptr);
	DTRACE_PROBE3(libpoireau, free, info.id, ptr, info.size);
	tracked_alloc_put(ptr);
	return;
}

EXPORT HOT NOINLINE void
free(void *ptr)
{

	if (UNLIKELY(tracked_alloc_p(ptr))) {
		sampled_free(ptr);
		return;
	}

	base_free(ptr);
	return;
}

static COLD NOINLINE void *
sampled_realloc_from_tracked(void *ptr, size_t request)
{
	struct tracked_alloc_info info;
	uint64_t new_id;
	void *ret;

	info = tracked_alloc_info(ptr);
	ret = tracked_alloc_get(request, &new_id);
	DTRACE_PROBE6(libpoireau, realloc_from_tracked,
	    info.id, ptr, info.size,
	    new_id, ret, request);

	if (ret == NULL)
		return ret;

	memcpy(ret, ptr, (info.size < request) ? info.size : request);
	tracked_alloc_put(ptr);
	return ret;
}

static ssize_t
safe_copy_one_chunk(pid_t self, void *dst, const void *src, size_t request)
{
	const struct iovec to[] = {
		{
			.iov_base = dst,
			.iov_len = request,
		},
	};
	const struct iovec from[] = {
		{
			.iov_base = (void *)src,
			.iov_len = request,
		},
	};

	return process_vm_readv(self, to, 1, from, 1, 0);
}

/**
 * Use process_vm_readv of the same process to copy page-aligned
 * chunks at a time.  We assume `dst` is fully writable, but `src`
 * might not be readable for the whole `request`.
 */
static void
safe_copy(void *dst, const void *src, size_t request)
{
	size_t copied;
	pid_t self;
	ssize_t r;

	self = getpid();
	/* Try one big copy. */
	r = safe_copy_one_chunk(self, dst, src, request);
	if ((size_t)r == request)
		return;

	/* If the request succeeded partially, take what we can. */
	if (r >= 0) {
		dst = (char *)dst + r;
		src = (const char *)src + r;
		request -= r;
	}

	/*
	 * Starting here, we only copy from one source page at a time.
	 *
	 * First, align `src` to a page boundary.
	 */
	{
		size_t max_initial_copy = PAGE_SIZE - ((uintptr_t)src % PAGE_SIZE);

		r = safe_copy_one_chunk(self, dst, src, max_initial_copy);
		if ((size_t)r != max_initial_copy)
			return;

		copied = max_initial_copy;
	}

	assert((((uintptr_t)src + copied) % PAGE_SIZE) == 0);

	while (copied < request) {
		size_t remaining = request - copied;
		size_t copy_size = PAGE_SIZE;

		assert((((uintptr_t)src + copied) % PAGE_SIZE) == 0);
		if (remaining < copy_size)
			copy_size = remaining;
		r = safe_copy_one_chunk(self, (char *)dst + copied,
		    (const char *)src + copied, copy_size);
		if ((size_t)r != copy_size)
			break;

		copied += copy_size;
	}

	return;
}

static COLD NOINLINE void *sampled_malloc(size_t request);

static COLD NOINLINE void *
sampled_realloc(void *ptr, size_t request)
{
	void *ret;
	uint64_t id;
	size_t old_size;

	if (sample_request_reset(&sample_state))
		return realloc(ptr, request);

	if (ptr == NULL)
		return sampled_malloc(request);

	if (tracked_alloc_p(ptr))
		return sampled_realloc_from_tracked(ptr, request);

	/*
	 * So, while we do have access to malloc_usable_size, its
	 * value should only be used for debugging or introspection.
	 * That function can return garbage, e.g., when glibc's malloc
	 * debugger is enabled.
	 *
	 * We'll instead copy with process_vm_readv.
	 */
	old_size = malloc_usable_size(ptr);
	ret = tracked_alloc_get(request, &id);

	DTRACE_PROBE5(libpoireau, realloc, ptr, old_size, id, ret, request);
	if (ret == NULL)
		return NULL;
	safe_copy(ret, ptr, request);
	base_free(ptr);
	return ret;
}

static COLD NOINLINE void *
sampled_realloc_to_regular(void *ptr, size_t request)
{
	struct tracked_alloc_info info;
	void *ret;

	info = tracked_alloc_info(ptr);
	ret = malloc(request);
	DTRACE_PROBE5(libpoireau, realloc_to_regular,
	    info.id, ptr, info.size, ret, request);
	if (ret == NULL)
		return ret;

	memcpy(ret, ptr, (info.size < request) ? info.size : request);
	tracked_alloc_put(ptr);
	return ret;
}

EXPORT HOT NOINLINE void *
realloc(void *ptr, size_t request)
{

	if (UNLIKELY(sample_request(&sample_state, request)))
		return sampled_realloc(ptr, request);

	if (UNLIKELY(tracked_alloc_p(ptr)))
		return sampled_realloc_to_regular(ptr, request);

	return base_realloc(ptr, request);
}

static COLD NOINLINE void *
sampled_malloc(size_t request)
{
	uint64_t id;
	void *ret;

	if (sample_request_reset(&sample_state))
		return malloc(request);

	ret = tracked_alloc_get(request, &id);
	DTRACE_PROBE3(libpoireau, malloc, id, ret, request);
	return ret;
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
	void *ret;
	uint64_t id;
	size_t req;

	if (sample_request_reset(&sample_state))
		return calloc(num, size);

	if (__builtin_umull_overflow(num, size, &req)) {
		DTRACE_PROBE2(libpoireau, calloc_overflow, num, size);
		return NULL;
	}

	ret = tracked_alloc_get(req, &id);
	DTRACE_PROBE5(libpoireau, calloc, num, size, id, ret, req);
	return ret;
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
