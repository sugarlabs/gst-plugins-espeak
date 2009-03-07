plugin_LTLIBRARIES = libgstespeak.la

libgstespeak_la_SOURCES = espeak.c gstespeak.c

libgstespeak_la_CFLAGS = $(GST_CFLAGS) $(GIO_CFLAGS) $(GST_AUDIO_CFLAGS)
libgstespeak_la_LIBADD = $(GST_LIBS) $(GIO_LIBS) $(GST_AUDIO_LIBS)
libgstespeak_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstespeak_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstespeak.h espeak.h
