#pragma once

/* SPDX-License-Identifier: MIT */

#define NOINLINE __attribute__((noinline, noclone))
#define LIKELY(X) (__builtin_expect(!!(X), 1))
#define UNLIKELY(X) (__builtin_expect(!!(X), 0))
#define EXPORT __attribute__((visibility("default")))
#define COLD __attribute__((cold))
#define HOT __attribute__((hot))
