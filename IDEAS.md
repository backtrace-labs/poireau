Some ideas for improvements
===========================

Testing
-------

### Generate a copy of the library without shadowing stdlib

### Confirm that `sample_request` has the correct distribution

### Test `tracked_alloc`'s ability to detect tracked allocs

### Confirm that `tracked_alloc` never hands out overlapping addresses

### Consider injecting faults?

Help detect heap corruption
---------------------------

### Let tracked allocations float by a couple pages away from 1G alignment

### Insert guard pages before and after tracked allocations

### Shift allocations to make them flush with the trailing guard page

### Supplement guard pages with noise filling to detect short overflows

### Add an quarantine queue

### Scan DieHard / DieHarder for more tricks

Improve statistical sampling
----------------------------

### Track the actual number of bytes before each sampled allocation

Using that be easier than correcting for large allocations.

### Generate from an Exponential without libm's log

Configurability
---------------

### Parse a configuration file for, e.g., the sample period

### Open a (abstract) unix socket for online configuration

### Vary sample rate based on dynamic context (e.g., hash(caller))

Improve usability
-----------------

### Real argument parsing in poireau.py

### Consider a small command shell in poireau.py

### Let poireau.py ingest stack trace patterns for known leaks

### Report a random number + pid in tracepoints

### Hook malloc-adjacent utilities like malloc_usable_size

### Hook posix_memalign, etc.

### Hook jemalloc/tcmalloc/etc. extensions

### Investigate need for fork hooks

Especially if we start listening on a unix socket.

### Dynamically size the allocation tracking table

### Automatically find a good age threshold

### Online streamgraph visualisation

### Flamegraph snapshot

### Flamegraph of average byte * second (time averaged snapshot)

Ask different questions
-----------------------

### Sample on pure call count?

### Detect allocation / deallocation pairs in different threads

Reduce overhead
----------------

### Fully disable allocation tracking until explicitly enabled

We want to PRELOAD with minimal overhead until we actually need the
tracking allocator.

### Explore ways to work better with static linking

List of candidates for related tool names
-----------------------------------------

- hasty (@stephentyrone)
