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

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <espeak/speak_lib.h>
#include <gst/gst.h>

#include "spin.h"
#include "espeak.h"

struct _Espeak
{
    Econtext *context;
    guint rate;
    guint pitch;
    const gchar *voice;
};

static gint espeak_sample_rate = 0;
static const espeak_VOICE **espeak_voices = NULL;
static GOutputStream *espeak_buffer = NULL;

static gint
synth_cb(short * data, int numsamples, espeak_EVENT * events)
{
    if (data == NULL)
        return 0;

    if (numsamples > 0)
        g_output_stream_write(espeak_buffer, data, numsamples*2, NULL, NULL);

    GST_DEBUG("numsamples=%d data_size=%ld", numsamples*2,
            g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(
                    espeak_buffer)));

    return 0;
}

static void
synth(const gchar *text, GMemoryOutputStream *sound, gpointer self_)
{
    Espeak *self = (Espeak*)self_;

    espeak_SetParameter(espeakPITCH, self->pitch, 0);
    espeak_SetParameter(espeakRATE, self->rate, 0);
    espeak_SetVoiceByName(self->voice);
    espeak_buffer = G_OUTPUT_STREAM(sound);

    espeak_Synth(text, strlen(text)+1, 0, POS_WORD, 0, espeakCHARS_UTF8,
            NULL, NULL);
}

static void
init()
{
    static volatile gsize initialized = 0;

    if (initialized == 0)
    {
        ++initialized;
        espeak_sample_rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 4096,
                NULL, 0);
        espeak_SetSynthCallback(synth_cb);
        espeak_voices = espeak_ListVoices(NULL);
        spin_init(synth);
    }
}

Espeak*
espeak_new()
{
    init();

    Espeak *self = g_new0(Espeak, 1);
    self->context = spin_new(self);
    self->pitch = ESPEAK_DEFAULT_PITCH;
    self->rate = ESPEAK_DEFAULT_RATE;
    self->voice = ESPEAK_DEFAULT_VOICE;

    return self;
}

void
espeak_unref(Espeak *self)
{
    spin_unref(self->context);
    memset(self, 0, sizeof(Espeak));
    g_free(self);
}

gint
espeak_get_sample_rate()
{
    return espeak_sample_rate;
}

gchar**
espeak_get_voices()
{
    gsize count = 0;
    const espeak_VOICE **i;
    char **j, **out;

    init();

    for (i = espeak_voices; *i; ++i) ++count;
    out = j = g_new0(gchar*, count); 
    for (i = espeak_voices; *i; ++i)
        *j++ = g_strconcat((*i)->name, ":", (*i)->languages+1, NULL);

    return out;
}

void
espeak_set_pitch(Espeak *self, guint value)
{
    self->pitch = value;
}

void
espeak_set_rate(Espeak *self, guint value)
{
    self->rate = value;
}

void
espeak_set_voice(Espeak *self, const gchar *value)
{
    self->voice = value;
}

void
espeak_say(Espeak *self, const gchar *text)
{
    spin_in(self->context, text);
}

gpointer
espeak_hear(Espeak *self, gsize size)
{
    return spin_out(self->context, &size);
}
