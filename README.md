Poireau: a sampling allocation debugger
=======================================

The libpoireau library intercepts a small fraction of calls to
malloc/calloc/etc., to generate a statistically representative
overview of an application's heap footprint.  While interception are
currently only tracks long-lived allocations (e.g., leaks), we plan to
also implement guard pages, in the spirit of
[Electric Fence](https://en.wikipedia.org/wiki/Electric_Fence).

How to build libpoireau
-----------------------

libpoireau currently targets Linux 4.8+ (for statically defined
tracepoint support) on 64 bit platforms.  Execute `make.sh` to create
`libpoireau.so` in the current directory; the code requires a
GCC-compatible C11 implementation.

How to use libpoireau
---------------------

Add `LD_PRELOAD="$LD_PRELOAD:$path_to_libpoireau.so"` to the
environment before executing the program you wish to debug.

Before using libpoireau, we must register its static probepoints with
Linux `perf`; this may be done before starting programs with
`LD_PRELOAD`, or after, it does not matter.

    sudo perf buildid-cache --add ./libpoireau.so
    sudo perf list | grep poireau  # should show tracepoints

We can now enable the tracepoints to generate perf events whenever
`libpoireau` overrides a stdlib call.

    sudo perf probe sdt_libpoireau:*
    sudo perf record -e sdt_libpoireau:* --call-graph=dwarf -p ...  # record events

Disable the tracepoints with

    sudo perf probe --del sdt_libpoireau:*

and remove libpoireau from `perf`'s cache with

    sudo perf buildid-cache --remove ./libpoireau.so

to erase all traces of libpoireau from the `perf` subsystem.

How does it work?
-----------------

When `LD_PRELOAD`ed, libpoireau intercepts every call to
`malloc`/`calloc`/`realloc`/`free`, and quickly forwards the vast
majority of calls to the real implementation that would be used if
libpoireau were absent.

Only those allocations that are marked for sampling are diverted, in
the case of `malloc` and `calloc`, and `free` is overridden iff called
on an allocation that was diverted.  Finally, `realloc` is treated as
a pair of `malloc` and `free`, for sampling purposes.

The sampling logic simulates a process that samples each allocated
byte with equal probability.  The (hardcoded) sampling rate aims for
an average of sampling one allocation every 32 MB; for example, we an
allocation request for 100 bytes becomes part of the sample with the
same probability as if we had flipped 100 times a biased coin that
lands on "head" with probability `1 / (32 * 1024 * 1024)`, and decided
to make the request part of the sample if any of these coin flip had
landed on "head."

This memory-less sampling strategy makes it possible to derive
statistical bounds on the shape of heap allocation calls, even with an
adversarial workload.  However, a naive implementation is slow.
Rather than flipping biased coins for each allocated byte, we instead
generate the number of consecutive "tails" results by generating
values from an Exponential distribution.

Whenever a call to `malloc`, `calloc`, or `realloc` is picked for
sampling, libpoireau executes code that is instrumented with USDT
(user statically-defined tracing) probes.  Linux `perf` can annotate
that code to generate events (this is a system-wide switch, for every
process that linked the shared library); we use these events to let
the kernel capture callstacks for each sampled call.

Vendored dependencies
---------------------

libpoireau includes code derived from
[xoroshiro 256+ 1.0](http://prng.di.unimi.it/xoshiro256plus.c),
written in 2018 by David Blackman and Sebastiano Vigna (vigna@acm.org)
and [dedicated to the public domain](http://creativecommons.org/publicdomain/zero/1.0/).

libpoireau includes Systemtap's `sys/sdt.h`, a file
[dedicated to the public domain](http://creativecommons.org/publicdomain/zero/1.0/).
