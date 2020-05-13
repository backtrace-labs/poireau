#!/bin/sh

BASE=$(dirname $(readlink -f "$0"))
SRC="$BASE/src"

SOURCES="$SRC/sample.c $SRC/shim.c $SRC/tracked_alloc.c"

LIB_SOURCES="$SRC/shim.c"

${CC:-cc} -std=gnu11 -D_GNU_SOURCE -g -ggdb -O2 -fPIC -W -Wall \
	  -flto -fvisibility=hidden -fno-semantic-interposition\
	  -ftls-model=initial-exec -I"$SRC" -isystem "$BASE/" $CFLAGS \
	  $SOURCES $LIB_SOURCE \
	  -ldl -lm --shared -o libpoireau.so

if [ ! -z "$BUILD_TESTS" ];
then
    for TEST in `ls test/*.c`;
    do
	${CC:-cc} -std=gnu11 -D_GNU_SOURCE -g -ggdb -O2 -fPIC -W -Wall \
		  -I"$SRC" -isystem "$BASE/" $CFLAGS $SOURCES $TEST \
		  -ldl -lm -o $(basename $TEST .c);
    done
fi
