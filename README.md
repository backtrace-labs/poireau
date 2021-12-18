Poireau: a sampling allocation debugger
=======================================

The libpoireau library intercepts a small fraction of calls to
malloc/calloc/etc., to generate a statistically representative
overview of an application's heap footprint.  While the interceptor
currently only tracks long-lived allocations (e.g., leaks), we plan to
also implement guard pages, in the spirit of
[Electric Fence](https://en.wikipedia.org/wiki/Electric_Fence).

The sampling approach makes it possible to use this library in
production with a minimal impact on performance (see the section on
Performance overhead), and without any change to code generation,
unlike, e.g., [LeakSanitizer](https://clang.llvm.org/docs/LeakSanitizer.html)
or [Valgrind](https://valgrind.org/).

The library's implementation strategy, which offloads most of the
complexity to the kernel or an external analysis script, and only
overrides the system memory allocator (or any other allocator that
already overrides the system malloc) for the few sampled allocations,
means the instrumentation is less likely to radically change a
program's behaviour.  Preloading `libpoireau.so` is much less invasive
than slotting in, e.g., [tcmalloc](https://github.com/google/tcmalloc)
only because one wants to debug allocations.  The code base is also
much smaller, and easier to audit before dropping a new library in
production.

Finally, rather than scanning the heap for references, the
`poireau.py` analysis script merely reports old allocations.  For
application servers, and other workloads that expect to enter a steady
state quickly after startup, that is *more useful* than only reporting
unreachable objects: a slow growth in heap footprint is an issue, even
if the culprits are reachable, e.g., in a list that isn't getting
cleared when it should be.

How to build libpoireau
-----------------------

libpoireau currently targets Linux 4.8+ (for statically defined
tracepoint support) on 64 bit platforms with 4 KB pages.  Execute
`make.sh` to create `libpoireau.so` in the current directory; the code
requires a GCC-compatible C11 implementation.

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

That's enough for Linux `perf` to report these events, e.g., in `perf
top`.  However, that's a lot of information, not necessarily useful.

Execute `scripts/poireau.sh $PID` to start `perf trace` on that `PID`,
and feed the output to an allocation tracking script.  Every 10
minutes, that script will dump a list of currently live old (> 5
minutes) sampled allocations.  Send `poireau.py` a `HUP` signal to
instead get a list of all live sampled allocations.  Old allocations
will eventually fill up with known leaks, or startup allocations;
remove all current old allocations from future reports by sending a
`USR1` signal to `poireau.py`.

A key advantage of having the analysis out of process is that we can
still provide information after a crash.  Send a `USR2` signal to
`poireau.py` to list some recent calls to `free` or `realloc`, on the
off chance that it will help debug a use-after-free.

Perf often needs `sudo` access, but it doesn't make sense to run all
of `poireau.py` as root; `poireau.sh` instead executes only `perf`
with sudo.  In order to override the `perf` binary under `sudo`,
use ``PERF=`which perf` scripts/poireau.sh ...``.

You may also enable system-wide tracing by invoking `poireau.sh`
without any argument.  This is mostly useful if only one process at a
time will ever `LD_PRELOAD` `libpoireau.so`: the analysis code in
`poireau.py` does not currently tell processes apart when matching
allocations and frees (edit the global `COMM` pattern in `poireau.py`
to only ingest events from programs that match a certain regex).
System-wide tracing makes it easier to track events that happen
immediately on program startup.

TL;DR:

1. Register `libpoireau`'s tracepoints with `perf buildid-cache --add`
   and `perf probe`.
2. Prepare your program to run with libpoireau.so instrumentation, e.g., with `LD_PRELOAD=/path/to/libpoireau.so`.
3. Grab libpoireau tracepoint events by doing one of:
   a. Start the instrumented program and run `scripts/poireau.sh $PROGRAM_PID`.
   b. Edit the `COMM` pattern in `scripts/poireau.py` before running `scripts/poireau.sh`, then start the instrumented program.
4. Wait for `poireau.sh` to report stacks for long-lived (> five minutes)
   sampled allocations, every ten minutes.
5. Packages for `perf` can be wonky. Try to build from source and point
   `poireau.sh` to custom executables by setting ``PERF=`which perf` ``
   before running `poireau.sh`.

Interact with `poireau.py` with signals:

- `SIGHUP`: prints stacks for all live sampled allocations.
- `SIGUSR1`: prints stacks for all old sampled allocations and stop reporting them in the future.
- `SIGUSR2`: prints stacks for all recent calls to `free` or `realloc`.

How to clean up after enabling libpoireau
-----------------------------------------

Disable the tracepoints with

    sudo perf probe --del sdt_libpoireau:*

and remove libpoireau from `perf`'s cache with

    sudo perf buildid-cache --remove ./libpoireau.so

to erase all traces of libpoireau from the `perf` subsystem.

If you had to edit an init script to insert the `LD_PRELOAD` variable
before executing a program, it makes sense to undo the edit and
restart the instrumented program as soon as possible.

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

In addition, these allocation requests are diverted to an internal
tracking allocator.  This lets us identify calls to `free` and
`realloc` on tracked allocations, which is crucial to generate paired
USDT events ("this allocation was freed or reallocated"); it also
ensures we pass these allocations back to the backup tracking
allocator, rather than the system malloc.

Some synthetic microbenchmarks
------------------------------

Performance sensitive programs tend to avoid dynamic memory allocation
in hot spots.  That being said, here are a couple microbenchmark to
try and upper bound the overhead of `LD_PRELOAD`ing in
`libpoireau.so`, by repeatedly making pairs of calls to `malloc` and
`free` (a best case for most memory allocators) in a single thread.
The results below were timed on an unloaded AMD EPYC 7601 running
Linux 5.3.11 and glibc 2.27.

Large allocations (1 MB), with a sample period of 32 MB (p = 3.2%):

    baseline (glibc malloc): 0.092 us/malloc-free (0.047 user, 0.046 system)
        preloaded, no probe: 0.153 us/malloc-free (0.058 user, 0.094 system)
     preloaded, with probes: 0.236 us/malloc-free (0.067 user, 0.169 system)
    preloaded, with tracing: 0.271 us/malloc-free (0.069 user, 0.203 system)

This is pretty much our worst case: we expect to trigger allocation
tracking very frequently, once every 32 allocation, and our tracking
allocator is slightly more complex than a plain `mmap`/`munmap`
(something we should still improve).

Mid-sized allocations (16 KB), with a sample period of 32 MB (p = 0.049%):

    baseline (glibc malloc): 0.042 us/malloc-free (0.041 user, 0.001 system)
        preloaded, no probe: 0.044 us/malloc-free (0.043 user, 0.001 system)
     preloaded, with probes: 0.046 us/malloc-free (0.042 user, 0.004 system)
    preloaded, with tracing: 0.054 us/malloc-free (0.042 user, 0.012 system)

At this less unreasonable size, the overhead of diverting sampled
allocations to a tracking allocator is less that 5%.  We can also
observe that, while triggering an interrupt whenever we execute a
tracepoint isn't free, the time spent servicing the interrupt is
relatively small (< 20%) compared to the time it takes to generate a
backtrace.  This isn't surprising, since we use the same part of the
kernel that's exercised when analysing performance issues with `perf`.

Small-sized allocations (128 B), with a sample period of 32 MB (p = 0.00038%):

    baseline (glibc malloc): 0.017 us/malloc-free (0.017 user, 0.000 system)
        preloaded, no probe: 0.020 us/malloc-free (0.020 user, 0.000 system)
     preloaded, with probes: 0.020 us/malloc-free (0.020 user, 0.000 system)
    preloaded, with tracing: 0.020 us/malloc-free (0.020 user, 0.000 system)

Here, all the slowdown is introduced by trampolining from our
interceptor malloc to the base system malloc.

TL;DR: in allocation microbenchmarks, the overhead of libpoireau
instrumentation is on the order of 5-20% for small or medium
allocations, and goes up to ~70% for very large allocations.

Enabling allocation tracing adds another 0-20% for small or medium
allocations, and ~130% for very large allocations.

These are worst-case figures, for a program that does *nothing* but
repeatedly `malloc` and `free` in a loop.  In practice, a performance
sensitive program hopefully spends less than 10% of its time in memory
management (and much less than that in large allocations), which means
the total overhead introduced by libpoireau and capturing stack traces
is probably closer to 1-5%.

Vendored dependencies
---------------------

libpoireau includes code derived from
[xoshiro 256+ 1.0](http://prng.di.unimi.it/xoshiro256plus.c),
written in 2018 by David Blackman and Sebastiano Vigna (vigna@acm.org)
and [dedicated to the public domain](http://creativecommons.org/publicdomain/zero/1.0/).

libpoireau includes Systemtap's `sys/sdt.h`, a file
[dedicated to the public domain](http://creativecommons.org/publicdomain/zero/1.0/).
