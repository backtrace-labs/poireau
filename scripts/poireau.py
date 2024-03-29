#!/usr/bin/env python3
"""Ingests the output of `perf trace -T -a -e sdt_libpoireau:* --call-graph=dwarf 2>&1`,
and prints live allocations on shutdown.

We first have to inform perf about libpoireau.so:
    sudo `which perf` buildid-cache  --add ./libpoireau.so
    sudo `which perf` probe sdt_libpoireau:*

We are now ready to track old allocations;
    sudo `which perf` trace -T -a -e sdt_libpoireau:* --call-graph=dwarf 2>&1 | ./poireau.py

The script will now report old allocations every 10 minutes.  You may
trigger a report by sending it a SIGHUP.  This report will eventually
be dominated by startup allocations or known leaks; mark all
allocations that would currently be reported as old with a SIGUSR1
(this will also report these allocations one last time).  Finally,
send a SIGUSR2 to print all recent free calls, which may be useful to
better understand use-after-free crashes.

And, to clean up,
    sudo `which perf` probe --del sdt_libpoireau:*
    sudo `which perf` buildid-cache  --remove ./libpoireau.so

It seems wasteful to make perf output a readable text format and parse
it in Python on the other end of a pipe, but, realistically, the real
work is in generating backtraces, not printing events: a pair of
`time` runs with and without dwarf stacktrace shows that walking and
symbolicating the stack is >95% of `perf trace`'s CPU time.

perf trace's output is a series of records of the form

 17602.282 coronerd/0/15464 sdt_libpoireau:malloc(__probe_ip: 140369085100327, arg1: 362, arg2: 139998753980416, arg3: 22605)
                                       sampled_malloc (/home/pkhuong/libpoireau/libpoireau.so)
                                       __cr_smr_malloc (/opt/backtrace/sbin/coronerd)
                                       string_create (/opt/backtrace/sbin/coronerd)
                                       string_open (/opt/backtrace/sbin/coronerd)
                                       _crdb_column_open (/opt/backtrace/sbin/coronerd)
                                       [0] ([unknown])
 17605.033 coronerd/0/15464 sdt_libpoireau:malloc(__probe_ip: 140369085100327, arg1: 363, arg2: 139997680238592, arg3: 68076)
                                       sampled_malloc (/home/pkhuong/libpoireau/libpoireau.so)
                                       __cr_smr_malloc (/opt/backtrace/sbin/coronerd)
                                       string_create (/opt/backtrace/sbin/coronerd)
                                       string_open (/opt/backtrace/sbin/coronerd)
                                       _crdb_column_open (/opt/backtrace/sbin/coronerd)
                                       [0] ([unknown])

Each record starts with a timestamp (`-T` ensures it's an absolute
value since system boot) in milliseconds; that's followed by the
comm/tid, then the trace event, with arguments.  The argument list
must be interpreted by each event handler (grep the source for DTRACE
static tracepoints to figure out their meaning).

After that comes the backtrace, which is apparently is a symbol
followed by a path in parentheses.

The parser is also compatible with the output of `perf script`.
We can thus run a command under `perf record`, e.g.,

`perf record -T -e sdt_libpoireau:* --call-graph=dwarf -- ./profilee`,

and analyse offline with `perf script | ./poireau.py`.
"""
from collections import defaultdict, namedtuple
from datetime import datetime
import fileinput
import os
import re
import sys
import signal
import time

# SPDX-License-Identifier: MIT

# Do we want to track sampled allocs when we hit the max heap footprint?
TRACK_HIGH_WATER_MARK = False

# If we do track sampled allocs, what's the minimum size we want
# before reporting anything?
TRACK_HIGH_WATER_MARK_AFTER = 0
if len(sys.argv) > 1 and sys.argv[1] == "--track-high-water-mark":
    TRACK_HIGH_WATER_MARK = True
    del sys.argv[1]
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        TRACK_HIGH_WATER_MARK_AFTER = int(sys.argv[1])
        del sys.argv[1]

# When reading from stdin, assume the records come from the same
# machine in real time, and we can use the monotonic clock to know the
# age of allocations.
RECORDS_MATCH_REAL_TIME = len(sys.argv) <= 1 or sys.argv[1] == "-"


# Assume libpoireau is configured with the same environment, or the
# default sampling period (32 MB).
ALLOCATION_SAMPLING_BYTE_PERIOD = os.environ.get("POIREAU_SAMPLE_PERIOD_BYTES", 32 << 20)

# Only track events with a comm (executable) that matches this pattern.
COMM_PATTERN = re.compile(".*")


# Periodically log when we process a parsable row.
LOG_ROW_PERIOD = 100 if RECORDS_MATCH_REAL_TIME else None


## Ingestion end of the pipeline: parse a stream of lines into a stream of Event tuples.
Frame = namedtuple("Frame", ["symbol", "dso"])

Event = namedtuple("Event", ["ts", "comm", "tid", "call", "stack"])


EVENT_PATTERN = re.compile(
    r"^\s*([0-9]+\.[0-9]*) (.*)/([0-9]+) (.*:.*[(].*[)])$", flags=re.ASCII
)

# 10332315.769 coronerd/110/12310 sdt_libpoireau:calloc:(7f7951f584f6) arg1=1 arg2=144 arg3=655 arg4=11957188952064 arg5=144
NEW_EVENT_PATTERN = re.compile(
    r"^(\s*[0-9]+\.[0-9]* .*/[0-9]+ .*:.*):\(([0-9a-f]+)\) ((arg[1-9][0-9]*=.+)*)$",
    flags=re.ASCII,
)

# coronerd/38 31909 [009] 627769.713769:               sdt_libpoireau:malloc: (7f4d353bd14d) arg1=8493 arg2=14291503677440 arg3=436
SCRIPT_EVENT_PATTERN = re.compile(
    r"^\s*(.*/[0-9]+) ([0-9]+) [[][0-9]+[]] ([0-9]+.[0-9]+):\s*(.*:.*):\s*\(([0-9a-f]+)\) ((arg[1-9][0-9]*=.+)*)$",
    flags=re.ASCII,
)


def parse_event(line):
    """Expects a new event line line
    '  17605.033 coronerd/0/15464 sdt_libpoireau:malloc(__probe_ip: 140369085100327, arg1: 363, arg2: 139997680238592, arg3: 68076)'
    and returns an Event tuple populated with the timestamp, comm, tid,
    and the remainder of the event line.

    The timestamp is converted to seconds.
    """
    match = EVENT_PATTERN.fullmatch(line)
    # New format... perf trace isn't exactly ABI stable.
    if match is None:
        match = NEW_EVENT_PATTERN.fullmatch(line)
        if not match:
            match = SCRIPT_EVENT_PATTERN.fullmatch(line)
            if match:
                line = f"{match[3]} {match[1]}/{match[2]} {match[4]}:({match[5]}) {match[6]}"
                match = NEW_EVENT_PATTERN.fullmatch(line)
        if match:
            args = ["__probe_ip=" + str(int(match[2], 16))] + match[3].split()
            args = [param.replace("=", ": ") for param in args]
            line = match[1] + "(" + ", ".join(args) + ")"

            match = EVENT_PATTERN.fullmatch(line)
    if match is None:
        print("Unhandled line: %s" % line, file=sys.stderr)
        return None
    return Event(float(match[1]) / 1000, match[2], int(match[3]), match[4], None)


FRAME_PATTERN = re.compile(r"^\s*([^0-9.].*) [(](.*)[)]$", flags=re.ASCII)

TRACE_FRAME_PATTERN = re.compile(
    r"^\s*[0-9a-f]{4,}\s+([^0-9.].*?)(:?[+]0x[0-9a-f]+)? [(](.*)[)]$", flags=re.ASCII
)


def parse_frame(line):
    """Expects a backtrace frame line like
    '                                       _crdb_column_open (/opt/backtrace/sbin/coronerd)'
    or
    '            7f4d353bd14d sampled_malloc+0x59 (/opt/backtrace/lib/libpoireau.so)'
    and returns a tuple of the symbol and path, or none if the line
    does not look like a stack trace frame.
    """
    match = TRACE_FRAME_PATTERN.fullmatch(line)
    if match is None:
        match = FRAME_PATTERN.fullmatch(line)
    if match is None:
        return None
    return Frame(match[1], match[2])


def segment_trace(lines):
    """Converts an iteratable of lines into an iterator of parsed Event
    tuples."""
    event = None
    stack = []
    for line in filter(None, map(lambda line: line.rstrip(), lines)):
        if event is None:  # we're looking for an event
            event = parse_event(line)
        else:
            frame = parse_frame(line)
            if frame:
                stack.append(frame)
            else:
                yield event._replace(stack=tuple(stack))
                event = parse_event(line)
                stack = []


## Middle end of the pipeline: parse event calls.
CALL_HANDLER_TABLE = dict()


def _make_pattern(name, num_arg):
    pattern = r"^sdt_libpoireau:" + name + "[(]"
    pattern += ", ".join(
        [r"__probe_ip: \d+"] + [r"arg%i: (\d+)" % i for i in range(1, num_arg + 1)]
    )
    pattern += "[)]$"
    return re.compile(pattern, flags=re.ASCII)


def call_handler(name, num_arg):
    """Use this decorator to register a handler for probe call `name` with `num_arg` arguments."""
    pattern = _make_pattern(name, num_arg)

    def pattern_decorator(fun):
        assert pattern not in CALL_HANDLER_TABLE
        CALL_HANDLER_TABLE[pattern] = fun
        return fun

    return pattern_decorator


# mmap_failed: size, alignment, padded_size, errno
@call_handler("mmap_failed", 4)
def mmap_failed_handler(match, event):
    print(
        "Application failed to mmap %s bytes (%s aligned to %s); errno %s. Trace: %s."
        % (match[3], match[1], match[2], match[4], event.stack),
        file=sys.stderr,
    )
    return None


# free: id, ptr, size
FreeCall = namedtuple("FreeCall", ["old_id", "old_ptr", "old_size"])


@call_handler("free", 3)
def free_handler(match, event):
    return FreeCall(int(match[1]), int(match[2]), int(match[3]))


# realloc_from_tracked: old_id, old_ptr, old_size, new_id, new_ptr, new_size
ReallocTrackedCall = namedtuple(
    "ReallocTrackedCall",
    ["old_id", "old_ptr", "old_size", "new_id", "new_ptr", "new_size"],
)


@call_handler("realloc_from_tracked", 6)
def realloc_tracked_handler(match, event):
    return ReallocTrackedCall(
        int(match[1]),
        int(match[2]),
        int(match[3]),
        int(match[4]),
        int(match[5]),
        int(match[6]),
    )


# realloc: old_ptr, old_size, new_id, new_ptr, new_size
ReallocUntrackedCall = namedtuple(
    "ReallocUntrackedCall", ["old_ptr", "old_size", "new_id", "new_ptr", "new_size"]
)


@call_handler("realloc", 5)
def realloc_untracked_handler(match, event):
    return ReallocUntrackedCall(
        int(match[1]), int(match[2]), int(match[3]), int(match[4]), int(match[5])
    )


# realloc_to_regular: old_id, old_ptr, old_size, new_ptr, new_size
ReallocLoseCall = namedtuple(
    "ReallocLoseCall", ["old_id", "old_ptr", "old_size", "new_ptr", "new_size"]
)


@call_handler("realloc_to_regular", 5)
def realloc_lose_handler(match, event):
    return ReallocLoseCall(
        int(match[1]), int(match[2]), int(match[3]), int(match[4]), int(match[5])
    )


# malloc: new_id, new_ptr, new_size
MallocCall = namedtuple("MallocCall", ["new_id", "new_ptr", "new_size"])


@call_handler("malloc", 3)
def malloc_handler(match, event):
    return MallocCall(int(match[1]), int(match[2]), int(match[3]))


# calloc_overflow: num, size
@call_handler("calloc_overflow", 2)
def calloc_overflow_handler(match, event):
    print(
        "Application failed to calloc %s * %s Trace: %s."
        % (match[1], match[2], event.stack),
        file=sys.stderr,
    )
    return None


# calloc: num, size, new_id, new_ptr, new_size
CallocCall = namedtuple(
    "CallocCall", ["elcount", "elsize", "new_id", "new_ptr", "new_size"]
)


@call_handler("calloc", 5)
def calloc_handler(match, event):
    return CallocCall(
        int(match[1]), int(match[2]), int(match[3]), int(match[4]), int(match[5])
    )


def parse_event_calls(events):
    event_type_count = defaultdict(int)
    for i, event in enumerate(events, 1):
        found = False
        result = None
        for pattern, handler in CALL_HANDLER_TABLE.items():
            match = pattern.fullmatch(event.call)
            if match:
                call = handler(match, event)
                if call is not None:
                    result = call
                    yield event._replace(call=call)
                found = True
                break
        if found:
            if result is not None:
                event_type_count[type(result).__name__] += 1
        else:
            event_type_count["Unknown"] += 1
            print("Unhandled call %s" % event.call, file=sys.stderr)
        if LOG_ROW_PERIOD and (i == 1 or (i % LOG_ROW_PERIOD) == 0):
            print(
                "%s processed %i events %s"
                % (datetime.utcnow().isoformat(), i, dict(event_type_count)),
                file=sys.stderr,
            )


## Model allocations and deallocations.  We were careful to name
## fields consistently: new_id/new_ptr/new_size for allocations,
## old_id/old_ptr/old_size for deallocations

Allocation = namedtuple(
    "Allocation",
    [
        "ptr",
        "size",
        "first_ts",
        "first_stack",
        "last_ts",
        "last_stack",
        "free_ts",
        "free_stack",
    ],
)

EMPTY_ALLOCATION = Allocation(*([None] * 8))

# We map allocations to buckets by dividing by 1 GB.
ALLOCATION_BUCKET_GRANULARITY = 1 << 30

# Keyed on allocation_bucket.  We should eventually look use
# tids and map to pids, but that's not necessary for now.
ALLOCATIONS = dict()

# Updated with the max timestamp we ever observed
LAST_EVENT = 0

# Estimate for the heap size in ALLOCATIONS
ESTIMATED_ALLOCATIONS_FOOTPRINT = 0

# Max for the heap size in ALLOCATIONS
ALLOCATIONS_HIGH_WATER_MARK = 0

# Updated with a list of live sampled allocations whenever we increase
# ALLOCATIONS_HIGH_WATER_MARK.
ALLOCATIONS_AT_HIGH_WATER_MARK = []


def estimate_allocation_size(alloc):
    """Estimates the actual allocation size that this sample represents.
    This is a very rough and biased estimate, but simple and hopefully
    enough to pinpoint memory bloat.
    """
    if not alloc.size:
        return 0
    return max(alloc.size, ALLOCATION_SAMPLING_BYTE_PERIOD)


def check_high_water_mark():
    global ALLOCATIONS_AT_HIGH_WATER_MARK, ALLOCATIONS_HIGH_WATER_MARK
    if (
        not TRACK_HIGH_WATER_MARK
        or ALLOCATIONS_HIGH_WATER_MARK >= ESTIMATED_ALLOCATIONS_FOOTPRINT
    ):
        return
    ALLOCATIONS_HIGH_WATER_MARK = ESTIMATED_ALLOCATIONS_FOOTPRINT
    ALLOCATIONS_AT_HIGH_WATER_MARK = [
        record for record in ALLOCATIONS.values() if not record.free_ts
    ]
    if ALLOCATIONS_HIGH_WATER_MARK >= TRACK_HIGH_WATER_MARK_AFTER:
        print_allocations_at_high_water_mark(True)


def assert_empty_bucket(key, alloc):
    current = ALLOCATIONS.get(key, None)
    # If we already have an allocation object that's not been freed
    # yet, something went really wrong.
    if current is not None and current.free_ts is None:
        print(
            "Heap corruption: double allocating in the same bucket?! old: %s, new: %s."
            % (current, alloc),
            file=sys.stderr,
        )


def assert_present_bucket(key, event):
    current = ALLOCATIONS.get(key, None)
    # If there is no current entry, we probably just started tracing
    # too late to observe the allocation call.
    if current is not None and current.free_ts is not None:
        print(
            "Double-free? de/re-allocating from an empty bucket?! old: %s, new: %f %s."
            % (current, event.ts, event.stack)
        )


def observe_alloc(event):
    global ESTIMATED_ALLOCATIONS_FOOTPRINT
    call = event.call
    key = call.new_id  # call.new_ptr // ALLOCATION_BUCKET_GRANULARITY
    alloc = ALLOCATIONS.get(key, EMPTY_ALLOCATION)
    alloc = alloc._replace(
        ptr=call.new_ptr, size=call.new_size, first_ts=event.ts, first_stack=event.stack
    )
    assert_empty_bucket(key, alloc)
    ALLOCATIONS[key] = alloc
    ESTIMATED_ALLOCATIONS_FOOTPRINT += estimate_allocation_size(alloc)
    check_high_water_mark()


def observe_free(event):
    global ESTIMATED_ALLOCATIONS_FOOTPRINT
    call = event.call
    key = call.old_id  # call.old_ptr // ALLOCATION_BUCKET_GRANULARITY
    assert_present_bucket(key, event)
    current = ALLOCATIONS.get(key, EMPTY_ALLOCATION)
    ALLOCATIONS[key] = current._replace(free_ts=event.ts, free_stack=event.stack)
    ESTIMATED_ALLOCATIONS_FOOTPRINT -= estimate_allocation_size(current)
    check_high_water_mark()


def observe_realloc(event):
    global ESTIMATED_ALLOCATIONS_FOOTPRINT
    call = event.call
    key = call.old_id  # old_ptr // ALLOCATION_BUCKET_GRANULARITY
    assert key == call.new_id  # call.new_ptr // ALLOCATION_BUCKET_GRANULARITY

    assert_present_bucket(key, event)
    current = ALLOCATIONS.get(key, EMPTY_ALLOCATION)
    new = current._replace(
        ptr=call.new_ptr, size=call.new_size, last_ts=event.ts, last_stack=event.stack,
    )
    ALLOCATIONS[key] = new
    ESTIMATED_ALLOCATIONS_FOOTPRINT -= estimate_allocation_size(current)
    ESTIMATED_ALLOCATIONS_FOOTPRINT += estimate_allocation_size(new)
    check_high_water_mark()


def observe_events(events):
    global LAST_EVENT
    for event in events:
        LAST_EVENT = max(LAST_EVENT, event.ts)
        if not re.match(COMM_PATTERN, event.comm):
            continue
        if (
            hasattr(event.call, "old_id")
            and hasattr(event.call, "new_id")
            and event.call.old_id == event.call.new_id
        ):
            observe_realloc(event)
        else:
            if hasattr(event.call, "old_id"):
                observe_free(event)
            if hasattr(event.call, "new_id"):
                observe_alloc(event)


## Print live allocations that were last touched 1s after the last event
def format_stack(stack):
    def format_frame(frame):
        # If the symbol is just an address, print the dso.
        if re.fullmatch(r"\[?(0x[0-9a-fA-F]+)|[0-9]+\]?", frame.symbol):
            return "[%s]" % frame.dso
        return frame.symbol

    return ";".join(map(format_frame, stack))


def print_alloc(alloc, now):
    """Prints a live allocation."""
    if alloc.last_ts is None:
        print(
            "\tsize=%i c_age=%f alloc=%s"
            % (alloc.size, now - alloc.first_ts, format_stack(alloc.first_stack))
        )
    else:
        print(
            "\tsize=%i c_age=%f alloc=%s m_age=%f realloc=%s"
            % (
                alloc.size,
                now - alloc.first_ts,
                format_stack(alloc.first_stack),
                now - alloc.last_ts,
                format_stack(alloc.last_stack),
            )
        )


def print_dalloc(alloc):
    """Prints freed allocation; useful immediately after a crash, to
    determine where a use-after-freed region was freed."""
    if alloc.last_ts is None:
        print(
            "\tptr=%x size=%i f_time=%f free=%s alloc=%s ctime=%f"
            % (
                alloc.ptr,
                alloc.size,
                alloc.free_ts,
                format_stack(alloc.free_stack),
                format_stack(alloc.first_stack),
                alloc.first_ts,
            )
        )
    else:
        print(
            "\tptr=%x size=%i ftime=%f free=%s alloc=%s realloc=%s ctime=%f mtime=%f"
            % (
                alloc.ptr,
                alloc.size,
                alloc.free_ts,
                format_stack(alloc.free_stack),
                format_stack(alloc.first_stack),
                format_stack(alloc.last_stack),
                alloc.first_ts,
                alloc.last_ts,
            )
        )


IGNORED_ALLOCS = set()


def print_allocations_at_high_water_mark(new_record):
    """Prints allocations in `allocs`, which should represent a set of
    allocations with a large aggregate footprint.

    Ignores any allocation that's already in IGNORED_ALLOCS; populates
    that set with all current allocations if mark_ignored is True.

    """
    if not TRACK_HIGH_WATER_MARK or not ALLOCATIONS_AT_HIGH_WATER_MARK:
        return
    if RECORDS_MATCH_REAL_TIME:
        now = time.monotonic()
        print(
            "%s allocations at %s high water mark %f MB"
            % (
                datetime.utcnow().isoformat(),
                "new" if new_record else "current",
                ALLOCATIONS_HIGH_WATER_MARK / (1 << 20),
            )
        )
    else:
        now = LAST_EVENT
        print(
            "allocations at %s high water mark %f MB"
            % (
                "new" if new_record else "current",
                ALLOCATIONS_HIGH_WATER_MARK / (1 << 20),
            )
        )

    # Print large allocations first.
    for alloc in sorted(
        ALLOCATIONS_AT_HIGH_WATER_MARK, key=lambda x: x.size, reverse=True
    ):
        if alloc.free_ts is None and alloc not in IGNORED_ALLOCS:
            print_alloc(alloc, now)


def print_old_allocs(allocs, max_age, max_stale=None, mark_ignored=False):
    """Prints allocations older than max_age.

    If max_stale is provided, it ignores allocations that were
    reallocated more recently than max_stale.

    Ignores any allocation that's already in IGNORED_ALLOCS; populates
    that set with all current allocations if mark_ignored is True.
    """
    global IGNORED_ALLOCS

    # We assume Allocation tuples do not come back in the list of
    # allocs; once it's disappeared from the list, we can drop it from
    # the ignored list.
    #
    # This set includes all Allocation tuples that should be ignored
    # and are still present in the list of allocs.
    seen_ignored = set()
    if RECORDS_MATCH_REAL_TIME:
        now = time.monotonic()
        print("%s old allocations" % datetime.utcnow().isoformat())
    else:
        now = LAST_EVENT

    for alloc in allocs:
        if alloc in IGNORED_ALLOCS or mark_ignored:
            seen_ignored.add(alloc)
        # It's been freed, clearly not a leak.
        if alloc.free_ts is not None:
            continue
        age = now - alloc.first_ts
        stale = now - alloc.last_ts if alloc.last_ts is not None else None
        # Don't print if it's been recently reallocated
        if stale and max_stale and stale <= max_stale:
            continue
        # If it's old and not ignored, print it.
        if age > max_age and alloc not in IGNORED_ALLOCS:
            print_alloc(alloc, now)
    IGNORED_ALLOCS = seen_ignored


def print_frees(allocs):
    """Dumps a list of all freed regions still in the tracking table."""
    if RECORDS_MATCH_REAL_TIME:
        print("%s recently freed regions" % datetime.utcnow().isoformat())
    else:
        print("Recently freed regions")
    for alloc in allocs:
        if alloc.free_ts is not None:
            print_dalloc(alloc)


# Report allocations older than this age in seconds
SUSPECT_ALLOCATION_AGE = 5 * 60
# And, if realloc, only do so after the realloc is this old
SUSPECT_ALLOCATION_STALE = 5 * 60

# Report old allocations at this period in seconds.
REPORT_PERIOD = 10 * 60

# The first timed report has a separate (usually lower) delay.
INITIAL_REPORT_DELAY = 30


def hup_handler(signum=None, frame=None):
    """On SIGHUP, print all current allocations."""
    print_old_allocs(ALLOCATIONS.values(), 0, max_stale=0)
    print_allocations_at_high_water_mark(False)


def usr1_handler(signum=None, frame=None):
    """On SIGUSR1, print old allocations, and add everything to the ignored set."""
    print_old_allocs(
        ALLOCATIONS.values(),
        SUSPECT_ALLOCATION_AGE,
        max_stale=SUSPECT_ALLOCATION_STALE,
        mark_ignored=True,
    )
    print(
        "%s currently extant allocations now ignored." % datetime.utcnow().isoformat()
    )


def usr2_handler(signum=None, frame=None):
    """On SIGUSR2, print freed allocations."""
    print_frees(ALLOCATIONS.values())


def alrm_handler(signum=None, frame=None):
    """Regulary print current old allocations"""
    print_old_allocs(
        ALLOCATIONS.values(), SUSPECT_ALLOCATION_AGE, max_stale=SUSPECT_ALLOCATION_STALE
    )
    signal.alarm(REPORT_PERIOD)


if __name__ == "__main__":
    signal.signal(signal.SIGALRM, alrm_handler)
    signal.signal(signal.SIGHUP, hup_handler)
    signal.signal(signal.SIGUSR1, usr1_handler)
    signal.signal(signal.SIGUSR2, usr2_handler)

    # There's no point sending out regular reports if they're not in
    # real time.
    if RECORDS_MATCH_REAL_TIME:
        # Kick off the initial report shortly after the time when an
        # allocation could be considered old enough to be suspect.
        signal.alarm(SUSPECT_ALLOCATION_AGE + INITIAL_REPORT_DELAY)
    try:
        observe_events(parse_event_calls(segment_trace(fileinput.input())))
    except KeyboardInterrupt:
        pass
    hup_handler()
    sys.exit(0)
