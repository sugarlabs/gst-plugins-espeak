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

struct Espeak
{
    GOutputStream *buffer;
};

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static GOutputStream *buffer = NULL;
static gint sample_rate = 0;
static const espeak_VOICE **voices = NULL;

static gint
read_cb(short * data, int numsamples, espeak_EVENT * events)
{
    if (data == NULL)
        return 0;

    if (numsamples > 0)
        g_output_stream_write(buffer, data, numsamples*2, NULL, NULL);

    return 0;
}

static void
init()
{
    static volatile gsize initialized = 0;

    pthread_mutex_lock(&mutex);
    if (initialized == 0)
    {
        ++initialized;
        sample_rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 4096, NULL, 0);
        espeak_SetSynthCallback(read_cb);
        voices = espeak_ListVoices(NULL);
    }
    pthread_mutex_unlock(&mutex);
}

struct Espeak*
espeak_new()
{
    init();

    if (sample_rate == EE_INTERNAL_ERROR)
        return NULL;

    struct Espeak *es = g_new(struct Espeak, 1);
    es->buffer = g_memory_output_stream_new(NULL, 0, realloc, free);

    return es;
}

gint
espeak_sample_rate()
{
    return sample_rate;
}

gchar**
espeak_voices()
{
    gsize count = 0;
    const espeak_VOICE **i;
    char **j, **out;

    init();

    for (i = voices; *i; ++i) ++count;
    out = j = g_new0(gchar*, count); 
    for (i = voices; *i; ++i)
        *j++ = g_strconcat((*i)->name, ":", (*i)->languages+1, NULL);

    return out;
}

gboolean
espeak_say(struct Espeak *es, const gchar *text, const gchar *voice,
        guint pitch, guint rate)
{
    g_seekable_seek(G_SEEKABLE(es->buffer), 0, G_SEEK_SET, NULL, NULL);

    pthread_mutex_lock(&mutex);
    buffer = es->buffer;
    espeak_SetParameter(espeakPITCH, pitch, 0);
    espeak_SetParameter(espeakRATE, rate, 0);
    espeak_SetVoiceByName(voice);
    gint status = espeak_Synth(text, strlen(text)+1, 0, POS_WORD, 0,
                espeakCHARS_AUTO, NULL, NULL);
    buffer = NULL;
    pthread_mutex_unlock(&mutex);

    if (status != EE_OK)
        return FALSE;

    return TRUE;
}

gpointer
espeak_hear(struct Espeak *es, goffset offset, guint *size)
{
    GMemoryOutputStream *mb = (GMemoryOutputStream*)es->buffer;

    gpointer out = g_memory_output_stream_get_data(mb) + offset;
    *size = MIN(g_memory_output_stream_get_data_size(mb) - offset, *size);

    return out;
}

void
espeak_unref(struct Espeak *es)
{
    g_output_stream_close(es->buffer, NULL, NULL);
    g_object_unref(es->buffer);
    es->buffer = 0;
    g_free(es);
}
