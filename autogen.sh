#!/bin/sh

srcdir=`dirname "$0"`
cd "$srcdir"
autoreconf -i -f
cd -
[ "$NOCONFIGURE" ] || "$srcdir"/configure CFLAGS="$CFLAGS -Wall" --enable-maintainer-mode "$@"
