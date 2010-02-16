plugin_LTLIBRARIES = libgstespeak.la

libgstespeak_la_SOURCES = espeak.c gstespeak.c

libgstespeak_la_CFLAGS = $(GST_CFLAGS) $(GST_AUDIO_CFLAGS) $(ESPEAK_CFLAGS)
libgstespeak_la_LIBADD = $(GST_LIBS) $(GST_AUDIO_LIBS) $(ESPEAK_LIBS)
libgstespeak_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstespeak_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstespeak.h espeak.h
