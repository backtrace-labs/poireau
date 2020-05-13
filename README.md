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

libpoireau currently targets Linux on 64 bit platforms.  Execute
`make.sh` to create `libpoireau.so` in the current directory; the code
requires a GCC-compatible C11 implementation.

How to use libpoireau
---------------------

Add `LD_PRELOAD="$LD_PRELOAD:$path_to_libpoireau.so"` to the
environment before executing the program you wish to debug.
