AC_INIT([gst-plugins-espeak], [0.5.0])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([configure.ac])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_HEADERS(config.h)

AM_MAINTAINER_MODE
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_PROG_CC
LT_INIT

GST_MAJORMINOR=1.0

PKG_CHECK_MODULES(GST, gstreamer-$GST_MAJORMINOR)
PKG_CHECK_MODULES(GST_AUDIO, gstreamer-audio-$GST_MAJORMINOR, have_audio=yes, have_audio=no)
if test "x$have_audio" = "xno"; then
    AC_CHECK_LIB(gstbase-$GST_MAJORMINOR, gst_base_src_get_type,, AC_MSG_ERROR())
    AC_CHECK_LIB(gstaudio-$GST_MAJORMINOR, gst_base_audio_src_get_type,, AC_MSG_ERROR())
fi

AC_CHECK_LIB(espeak-ng, espeak_Initialize,, AC_MSG_ERROR())

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
GST_PLUGIN_LDFLAGS="-module -avoid-version $GST_ALL_LDFLAGS"
AC_SUBST(GST_PLUGIN_LDFLAGS)

AC_CONFIG_FILES([Makefile m4/Makefile src/Makefile])
AC_OUTPUT
