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

/**
 * SECTION:element-espeak
 *
 * Use espeak as a sound source.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-0.10 espeak text="Hello world" ! autoaudiosink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include <string.h>

#include "gstespeak.h"
#include "espeak.h"

GST_DEBUG_CATEGORY_STATIC (gst_espeak_debug);
#define GST_CAT_DEFAULT gst_espeak_debug

enum
{
    PROP_0,
    PROP_TEXT,
    PROP_PITCH,
    PROP_RATE,
    PROP_VOICE,
    PROP_GAP,
    PROP_TRACK,
    PROP_VOICES,
    PROP_CAPS
};

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE (
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

static GstFlowReturn gst_espeak_create(GstBaseSrc*,
    guint64, guint, GstBuffer**);
static gboolean gst_espeak_start(GstBaseSrc*);
static gboolean gst_espeak_stop(GstBaseSrc*);
static gboolean gst_espeak_is_seekable (GstBaseSrc*);
static void gst_espeak_init_uri(GType);
static void gst_espeak_finalize(GObject*);
static void gst_espeak_set_property(GObject*, guint, const GValue*,
        GParamSpec*);
static void gst_espeak_get_property(GObject*, guint, GValue*, GParamSpec*);
static GstCaps *gst_espeak_getcaps(GstBaseSrc*);

GST_BOILERPLATE_FULL(GstEspeak, gst_espeak, GstBaseSrc, GST_TYPE_BASE_SRC,
        gst_espeak_init_uri);

/******************************************************************************/

static void
gst_espeak_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);

  gst_element_class_set_details_simple(element_class,
    "Espeak",
    "FIXME:Generic",
    "FIXME:Generic Template Element",
    " <<user@hostname.org>>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));
}

/* initialize the espeak's class */
static void
gst_espeak_class_init(GstEspeakClass * klass)
{
    GObjectClass *gobject_class = (GObjectClass *) klass;
    GstBaseSrcClass *basesrc_class = (GstBaseSrcClass *) klass;

    basesrc_class->create = gst_espeak_create;
    basesrc_class->start = gst_espeak_start;
    basesrc_class->stop = gst_espeak_stop;
    basesrc_class->is_seekable = gst_espeak_is_seekable;
    basesrc_class->get_caps = gst_espeak_getcaps;

    gobject_class->finalize = gst_espeak_finalize;
    gobject_class->set_property = gst_espeak_set_property;
    gobject_class->get_property = gst_espeak_get_property;

    g_object_class_install_property(gobject_class, PROP_TEXT,
            g_param_spec_string("text", "Text",
                "Text to pronounce", NULL,
                G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_PITCH,
            g_param_spec_uint("pitch", "Pitch adjustment",
                "Pitch adjustment", 0, 99, ESPEAK_DEFAULT_PITCH,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_RATE,
            g_param_spec_uint("rate", "Speed in words per minute",
                "Speed in words per minute", 80, 390, ESPEAK_DEFAULT_RATE,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_VOICE,
            g_param_spec_string("voice", "Current voice",
                "Current voice", ESPEAK_DEFAULT_VOICE,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_GAP,
            g_param_spec_uint("gap", "Gap",
                "Word gap", 0, G_MAXINT, ESPEAK_DEFAULT_GAP,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_TRACK,
            g_param_spec_uint("track", "Track",
                "Track espeak events", 0, G_MAXINT, ESPEAK_TRACK_NONE,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_VOICES,
            g_param_spec_boxed("voices", "List of voices",
                "List of voices", G_TYPE_VALUE_ARRAY,
                G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_CAPS,
            g_param_spec_boxed("caps", "Caps",
                "Caps describing the format of the data", GST_TYPE_CAPS,
                G_PARAM_READABLE));
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_espeak_init (GstEspeak * self,
    GstEspeakClass * gclass)
{
    self->pitch = ESPEAK_DEFAULT_PITCH;
    self->rate = ESPEAK_DEFAULT_RATE;
    self->voice = g_strdup(ESPEAK_DEFAULT_VOICE);
    self->voices = espeak_get_voices();
    self->speak = espeak_new(GST_ELEMENT(self));

    self->caps = gst_caps_new_simple("audio/x-raw-int",
            "rate", G_TYPE_INT, espeak_get_sample_rate(),
            "channels", G_TYPE_INT, 1,
            "endianness", G_TYPE_INT, G_BYTE_ORDER,
            "width", G_TYPE_INT, 16,
            "depth", G_TYPE_INT, 16,
            "signed", G_TYPE_BOOLEAN, TRUE,
            NULL);
}

static void
gst_espeak_finalize(GObject * self_)
{
fprintf(stderr, "0!!!!!!\n");
    GstEspeak *self = GST_ESPEAK(self_);

    gst_caps_unref(self->caps);         self->caps = NULL;
    espeak_unref(self->speak);          self->speak = NULL;
    g_free(self->voice);                self->voice = NULL;
    g_value_array_free(self->voices);   self->voices = NULL;

    G_OBJECT_CLASS(parent_class)->dispose(self_);
}

/******************************************************************************/

static void
gst_espeak_set_property(GObject *object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
    GstEspeak *self = GST_ESPEAK(object);

    switch (prop_id) {
        case PROP_TEXT:
            espeak_in(self->speak, g_value_get_string(value));
            break;
        case PROP_PITCH:
            self->pitch = g_value_get_uint(value);
            espeak_set_pitch(self->speak, self->pitch);
            break;
        case PROP_RATE:
            self->rate = g_value_get_uint(value);
            espeak_set_rate(self->speak, self->rate);
            break;
        case PROP_VOICE:
            self->voice = g_strdup(g_value_get_string(value));
            espeak_set_voice(self->speak, self->voice);
            break;
        case PROP_GAP:
            self->gap = g_value_get_uint(value);
            espeak_set_gap(self->speak, self->gap);
            break;
        case PROP_TRACK:
            self->track = g_value_get_uint(value);
            espeak_set_track(self->speak, self->track);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
gst_espeak_get_property(GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
    GstEspeak *self = GST_ESPEAK(object);

    switch (prop_id) {
        case PROP_PITCH:
            g_value_set_uint(value, self->pitch);
            break;
        case PROP_RATE:
            g_value_set_uint(value, self->rate);
            break;
        case PROP_VOICE:
            g_value_set_string(value, self->voice);
            break;
        case PROP_GAP:
            g_value_set_uint(value, self->gap);
            break;
        case PROP_TRACK:
            g_value_set_uint(value, self->track);
            break;
        case PROP_VOICES:
            g_value_set_boxed(value, self->voices);
            break;
        case PROP_CAPS:
            gst_value_set_caps(value, self->caps);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

/******************************************************************************/

static GstFlowReturn
gst_espeak_create(GstBaseSrc * self_, guint64 offset, guint size,
        GstBuffer **buf)
{
    GstEspeak *self = GST_ESPEAK(self_);

    *buf = espeak_out(self->speak, size);

    if (*buf)
        return GST_FLOW_OK;
    else
    {
        //gst_element_set_state(GST_ELEMENT(self), GST_STATE_NULL);
        return GST_FLOW_UNEXPECTED;
    }
}

static gboolean
gst_espeak_start(GstBaseSrc * self_)
{
    return TRUE;
}

static gboolean
gst_espeak_stop(GstBaseSrc * self)
{
    GST_DEBUG("!!!!!!!!!!");
    return TRUE;
}

static gboolean
gst_espeak_is_seekable(GstBaseSrc * src)
{
    return FALSE;
}

static GstCaps *
gst_espeak_getcaps(GstBaseSrc *self_)
{
    GstEspeak *self = GST_ESPEAK(self_);
    return gst_caps_ref(self->caps);
}

/******************************************************************************/

static GstURIType
gst_espeak_uri_get_type(void)
{
    return GST_URI_SRC;
}

static gchar**
gst_espeak_uri_get_protocols(void)
{
    static gchar *protocols[] = { "espeak", NULL };
    return protocols;
}

static gboolean
gst_espeak_uri_set_uri(GstURIHandler *handler, const gchar *uri)
{
    gchar *protocol, *text;
    gboolean ret;

    protocol = gst_uri_get_protocol(uri);
    ret = strcmp(protocol, "espeak") == 0;
    g_free(protocol);
    if (!ret) return FALSE;

    text = gst_uri_get_location(uri);

    if (!text)
        return FALSE;

    espeak_in(GST_ESPEAK(handler)->speak, text);
    g_free (text);

    return TRUE;
}

static void
gst_espeak_uri_handler_init(gpointer g_iface, gpointer iface_data)
{
    GstURIHandlerInterface *iface = (GstURIHandlerInterface*) g_iface;

    iface->get_type = gst_espeak_uri_get_type;
    iface->get_protocols = gst_espeak_uri_get_protocols;
    iface->set_uri = gst_espeak_uri_set_uri;
}

static void
gst_espeak_init_uri(GType filesrc_type)
{
    static const GInterfaceInfo urihandler_info = {
            gst_espeak_uri_handler_init,
            NULL,
            NULL
    };
    g_type_add_interface_static (filesrc_type, GST_TYPE_URI_HANDLER,
            &urihandler_info);
}

/******************************************************************************/

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
espeak_init(GstPlugin *espeak)
{
  /* debug category for fltering log messages
   *
   * exchange the string 'Template espeak' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_espeak_debug, "espeak",
      0, "Template espeak");

  return gst_element_register (espeak, "espeak", GST_RANK_NONE,
      GST_TYPE_ESPEAK);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "espeak",
    "Uses eSpeak library as a sound source for GStreamer",
    espeak_init,
    PACKAGE_VERSION,
    "LGPL",
    PACKAGE_NAME,
    "http://sugarlabs.org/go/DevelopmentTeam/gst-plugins-espeak"
)
