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

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <espeak/speak_lib.h>

struct Espeak
{
    GOutputStream *buffer;
};

static unsigned char wave_hdr[44] = {
    'R','I','F','F',0x24,0xf0,0xff,0x7f,'W','A','V','E','f','m','t',' ',
    0x10,0,0,0,1,0,1,0,  9,0x3d,0,0,0x12,0x7a,0,0,
    2,0,0x10,0,'d','a','t','a',  0x00,0xf0,0xff,0x7f};

static GMutex *mutex = NULL;
static GOutputStream *buffer = NULL;
static gint rate = 0;

static gint
read_cb(short * wav, int numsamples, espeak_EVENT * events)
{
    if (wav == NULL)
        return 0;

    if (numsamples > 0)
        g_output_stream_write(buffer, wav, numsamples*2, NULL, NULL);

    return 0;
}

struct Espeak*
espeak_new()
{
    static gsize initialized = 0;

    if (g_once_init_enter(&initialized))
    {
        rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 4096, NULL, 0);
        espeak_SetSynthCallback(read_cb);
        mutex = g_mutex_new();
    }

    if (rate == EE_INTERNAL_ERROR)
        return NULL;

    struct Espeak *es = g_new(struct Espeak, 1);
    es->buffer = g_memory_output_stream_new(NULL, 0, realloc, free);

    return es;
}

gboolean
espeak_say(struct Espeak *es, const gchar *text)
{
    void write4bytes(GOutputStream *buffer, gint value)
    {
        gint i, byte;

        for(i = 4; i--;)
        {
            byte = value & 0xff;
            g_output_stream_write(buffer, &byte, 1, NULL, NULL);
            value >>= 8;
        }
    }

    g_seekable_seek((GSeekable*)es->buffer, 0, G_SEEK_SET, NULL, NULL);

    g_output_stream_write(es->buffer, wave_hdr, 24, NULL, NULL);
    write4bytes(es->buffer, rate);
    write4bytes(es->buffer, rate * 2);
    g_output_stream_write(es->buffer, wave_hdr+32, 12, NULL, NULL);

    g_mutex_lock(mutex);
    buffer = es->buffer;
    gint status = espeak_Synth(text, strlen(text), 0, POS_CHARACTER, 0,
                espeakCHARS_AUTO, NULL, NULL);
    buffer = NULL;
    g_mutex_unlock(mutex);

    return status == EE_OK;
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
