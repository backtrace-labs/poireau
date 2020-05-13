Poireau: a sampling allocation debugger
=======================================

The libpoireau library intercepts a small fraction of calls to
malloc/calloc/etc., to generate a statistically representative
overview of an application's heap footprint.  While interception are
currently only tracks long-lived allocations (e.g., leaks), we plan to
also implement guard pages, in the spirit of
[Electric Fence](https://en.wikipedia.org/wiki/Electric_Fence).
