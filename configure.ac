AC_INIT([gst-plugins-espeak], [0.4.0])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([configure.ac])
AM_INIT_AUTOMAKE([foreign])
AM_CONFIG_HEADER(config.h)

AM_MAINTAINER_MODE
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_PROG_CC
AC_PROG_LIBTOOL

GST_MAJORMINOR=1.0

PKG_CHECK_MODULES(GST, gstreamer-$GST_MAJORMINOR)
PKG_CHECK_MODULES(GST_AUDIO, gstreamer-audio-$GST_MAJORMINOR, have_audio=yes, have_audio=no)
if test "x$have_audio" = "xno"; then
    AC_CHECK_LIB(gstbase-$GST_MAJORMINOR, gst_base_src_get_type,, AC_MSG_ERROR())
    AC_CHECK_LIB(gstaudio-$GST_MAJORMINOR, gst_base_audio_src_get_type,, AC_MSG_ERROR())
fi

dnl Check for 0sugar support
if test "${ESPEAK_PREFIX}"; then
    ESPEAK_CFLAGS="-I${ESPEAK_PREFIX}/include"
    ESPEAK_LIBS="-L${ESPEAK_PREFIX}/lib -lespeak"
    AC_SUBST(ESPEAK_CFLAGS)
    AC_SUBST(ESPEAK_LIBS)
else
    AC_CHECK_LIB(espeak, espeak_Initialize,, AC_MSG_ERROR())
fi

if test "x${prefix}" = "x$HOME"; then
  plugindir="$HOME/.gstreamer-$GST_MAJORMINOR/plugins"
else
  plugindir="\$(libdir)/gstreamer-$GST_MAJORMINOR"
fi
AC_SUBST(plugindir)

dnl LDFLAGS really should only contain flags, not libs - they get added before
dnl whatevertarget_LIBS and -L flags here affect the rest of the linking
GST_ALL_LDFLAGS="-no-undefined"
AC_SUBST(GST_ALL_LDFLAGS)

dnl set proper LDFLAGS for plugins
GST_PLUGIN_LDFLAGS="-module -avoid-version -export-symbols-regex '^_*gst_plugin_desc\$\$' $GST_ALL_LDFLAGS"
AC_SUBST(GST_PLUGIN_LDFLAGS)

AC_OUTPUT(Makefile m4/Makefile src/Makefile)
