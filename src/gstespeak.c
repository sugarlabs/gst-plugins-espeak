/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2009 Aleksey S. Lim <alsroot@member.fsf.org>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
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
 * FIXME:Describe espeak here.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v -m fakesrc ! espeak ! fakesink silent=TRUE
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
    PROP_RATE
};

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE (
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("ANY")
    );

static GstFlowReturn gst_espeak_create (GstBaseSrc*,
    guint64, guint, GstBuffer**);
static gboolean gst_espeak_start (GstBaseSrc*);
static gboolean gst_espeak_stop (GstBaseSrc*);
static gboolean gst_espeak_is_seekable (GstBaseSrc*);
static gboolean gst_espeak_unlock (GstBaseSrc*);
static gboolean gst_espeak_unlock_stop (GstBaseSrc*);
static gboolean gst_espeak_do_seek (GstBaseSrc*, GstSegment*);
static gboolean gst_espeak_check_get_range (GstBaseSrc*);
static gboolean gst_espeak_do_get_size (GstBaseSrc*, guint64*);
static void gst_espeak_init_uri(GType);
static void gst_espeak_finalize(GObject * gobject);
static void gst_espeak_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_espeak_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

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
gst_espeak_class_init (GstEspeakClass * klass)
{
    GObjectClass *gobject_class = (GObjectClass *) klass;
    GstBaseSrcClass *basesrc_class = (GstBaseSrcClass *) klass;

    basesrc_class->create = gst_espeak_create;
    basesrc_class->start = gst_espeak_start;
    basesrc_class->stop = gst_espeak_stop;
    basesrc_class->stop = gst_espeak_stop;
    basesrc_class->is_seekable = gst_espeak_is_seekable;
    basesrc_class->unlock = gst_espeak_unlock;
    basesrc_class->unlock_stop = gst_espeak_unlock_stop;
    basesrc_class->do_seek = gst_espeak_do_seek;
    basesrc_class->check_get_range = gst_espeak_check_get_range;
    basesrc_class->get_size = gst_espeak_do_get_size;

    gobject_class->finalize = gst_espeak_finalize;
    gobject_class->set_property = gst_espeak_set_property;
    gobject_class->get_property = gst_espeak_get_property;

    g_object_class_install_property(gobject_class, PROP_TEXT,
            g_param_spec_string("text", "Text",
                "Text to pronounce", NULL,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_PITCH,
            g_param_spec_uint("pitch", "Pitch adjustment",
                "Pitch adjustment", 0, 99, 50,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_RATE,
            g_param_spec_uint("rate", "Speed in words per minute",
                "Speed in words per minute", 80, 390, 170,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

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
    self->text = NULL;
    self->uri = NULL;
    self->speak = espeak_new();
    self->pitch = 50;
    self->rate = 170;
}

static void
gst_espeak_finalize(GObject * self_)
{
    GstEspeak *self = (GstEspeak*)self_;

    espeak_unref(self->speak);  self->speak = NULL;
    g_free(self->text);         self->text = NULL;
    g_free(self->uri);          self->uri = NULL;

    G_OBJECT_CLASS(parent_class)->dispose(self_);
}

/******************************************************************************/

static gboolean
gst_espeak_set_text(GstEspeak *self, const gchar *text)
{
    GstState state;

    /* the element must be stopped in order to do this */
    GST_OBJECT_LOCK(self);
    state = GST_STATE(self);
    GST_OBJECT_UNLOCK (self);

    if (state != GST_STATE_READY && state != GST_STATE_NULL)
    {
        GST_DEBUG_OBJECT(self, "setting text in wrong state");
        return FALSE;
    }

    g_free(self->text);
    g_free(self->uri);

    if (text == NULL) {
        self->text = NULL;
        self->uri = NULL;
    } else {
        self->text = g_strdup(text);
        self->uri = gst_uri_construct ("espeak", self->text);
    }

    g_object_notify(G_OBJECT (self), "text");
    gst_uri_handler_new_uri(GST_URI_HANDLER(self), self->uri);

    return TRUE;
}

static void
gst_espeak_set_property (GObject *object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
    GstEspeak *self = GST_ESPEAK(object);

    switch (prop_id) {
        case PROP_TEXT:
            gst_espeak_set_text(self, g_value_get_string(value));
            break;
        case PROP_PITCH:
            self->pitch = g_value_get_uint(value);
            break;
        case PROP_RATE:
            self->rate = g_value_get_uint(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
gst_espeak_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
    GstEspeak *self = GST_ESPEAK(object);

    switch (prop_id) {
        case PROP_TEXT:
            g_value_set_string(value, self->text);
            break;
        case PROP_PITCH:
            g_value_set_uint(value, self->pitch);
            break;
        case PROP_RATE:
            g_value_set_uint(value, self->rate);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

/******************************************************************************/

static GstFlowReturn
gst_espeak_create (GstBaseSrc * self_, guint64 offset, guint size,
        GstBuffer ** buf)
{
    GstEspeak *self = (GstEspeak*)self_;
    *buf = gst_buffer_new();
    GST_BUFFER_DATA (*buf) = espeak_hear(self->speak, offset, &size);
    GST_BUFFER_SIZE (*buf) = size;
    return size == 0 ? GST_FLOW_UNEXPECTED : GST_FLOW_OK;
}

static gboolean
gst_espeak_start (GstBaseSrc * self_)
{
    GstEspeak *self = GST_ESPEAK(self_);

    if (self->text == NULL || self->text[0] == 0)
        return FALSE;

    return espeak_say(self->speak, self->text, self->pitch, self->rate);
}

static gboolean
gst_espeak_stop (GstBaseSrc * self)
{
    return TRUE;
}

static gboolean
gst_espeak_is_seekable (GstBaseSrc * src)
{
    return FALSE;
}

static gboolean gst_espeak_unlock (GstBaseSrc * bsrc)
{
    return TRUE;
}

static gboolean gst_espeak_unlock_stop (GstBaseSrc * bsrc)
{
    return TRUE;
}

static gboolean gst_espeak_do_seek (GstBaseSrc * src, GstSegment * segment)
{
    return TRUE;
}

static gboolean gst_espeak_check_get_range (GstBaseSrc * src)
{
    return FALSE;
}

static gboolean gst_espeak_do_get_size (GstBaseSrc * src, guint64 * size)
{
    *size = -1;
    return TRUE;
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

static const gchar *
gst_espeak_uri_get_uri(GstURIHandler *handler)
{
    GstEspeak *self = GST_ESPEAK(handler);
    return self->uri;
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

    ret = gst_espeak_set_text(GST_ESPEAK(handler), text);
    g_free (text);

    return ret;
}

static void
gst_espeak_uri_handler_init(gpointer g_iface, gpointer iface_data)
{
    GstURIHandlerInterface *iface = (GstURIHandlerInterface*) g_iface;

    iface->get_type = gst_espeak_uri_get_type;
    iface->get_protocols = gst_espeak_uri_get_protocols;
    iface->get_uri = gst_espeak_uri_get_uri;
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
espeak_init (GstPlugin * espeak)
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

/* gstreamer looks for this structure to register espeaks
 *
 * exchange the string 'Template espeak' with your espeak description
 */
GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "espeak",
    "Template espeak",
    espeak_init,
    VERSION,
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)
