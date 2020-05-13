#!/bin/sh

# SPDX-License-Identifier: MIT

BASE=$(dirname $(readlink -f "$0"))

if [ -z "$1" ]
then
    TARGET="-a"
else
    TARGET="-p $1"
fi

sudo ${PERF:-perf} trace -T $TARGET \
     -e sdt_libpoireau:* --call-graph=dwarf 2>&1 |  \
    "${BASE}/poireau.py"
