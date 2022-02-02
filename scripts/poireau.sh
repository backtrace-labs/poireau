#!/bin/sh

# SPDX-License-Identifier: MIT

BASE=$(dirname $(readlink -f "$0"))

if [ -z "$1" ]
then
    TARGET="-a"
elif [ "$1" = "*" ]
then
    TARGET="-a"
    shift
else
    TARGET="-p $1"
    shift
fi

sudo ${PERF:-perf} trace -T $TARGET \
     -e sdt_libpoireau:* --call-graph=dwarf --sort-events 2>&1 |  \
    "${BASE}/poireau.py" "$@"
