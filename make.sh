#!/bin/sh

BASE=$(dirname $(readlink -f "$0"))
SRC="$BASE/src"

SOURCES="$SRC/shim.c"

${CC:-cc} -std=gnu11 -D_GNU_SOURCE -g -ggdb -O2 -fPIC -W -Wall \
	  -flto -fvisibility=hidden -fno-semantic-interposition\
	  -ftls-model=initial-exec -I"$SRC" $CFLAGS $SOURCES \
	  -ldl -lm --shared -o libpoireau.so
