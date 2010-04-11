#!/bin/sh

aclocal -I m4/ $ACLOCAL_FLAGS --install || exit 1
libtoolize --copy --force || exit 1
autoheader || exit 1
autoconf || exit 1
automake -a -c || exit 1

[ "$NOCONFIGURE" ] || ./configure CFLAGS="$CFLAGS -Wall" --enable-maintainer-mode
