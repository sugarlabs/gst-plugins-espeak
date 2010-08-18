/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GST_ESPEAK_H__
#define __GST_ESPEAK_H__

#include <gst/gst.h>
#include <gst/audio/gstaudiosrc.h>

G_BEGIN_DECLS
/* #defines don't like whitespacey bits */
#define GST_TYPE_ESPEAK \
  (gst_espeak_get_type())
#define GST_ESPEAK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_ESPEAK,GstEspeak))
#define GST_ESPEAK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_ESPEAK,GstEspeakClass))
#define GST_IS_ESPEAK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_ESPEAK))
#define GST_IS_ESPEAK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_ESPEAK))
typedef struct _GstEspeak GstEspeak;
typedef struct _GstEspeakClass GstEspeakClass;
struct _Econtext;

struct _GstEspeak {
    GstAudioSrc parent;
    struct _Econtext *speak;
    gchar *text;
    gint pitch;
    gint rate;
    gchar *voice;
    guint gap;
    guint track;
    GValueArray *voices;
    GstCaps *caps;
    gboolean poll;
};

struct _GstEspeakClass {
    GstAudioSrcClass parent_class;
};

GType gst_espeak_get_type (void);

G_END_DECLS
#endif /* __GST_ESPEAK_H__ */
