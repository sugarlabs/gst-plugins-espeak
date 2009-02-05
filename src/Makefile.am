plugin_LTLIBRARIES = libgstespeak.la

libgstespeak_la_SOURCES = gstespeak.c espeak.c

libgstespeak_la_CFLAGS = $(GST_CFLAGS)
libgstespeak_la_LIBADD = $(GST_LIBS) -lespeak -lgio-2.0 -lgobject-2.0 -lgmodule-2.0 -lglib-2.0 -lgstaudio-0.10 -lgstbase-0.10 -lgstinterfaces-0.10 -lgstreamer-0.10
libgstespeak_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstespeak_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstespeak.h espeak.h
