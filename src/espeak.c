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

#include <unistd.h>


#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <espeak/speak_lib.h>

#define SYNC_BUFFER_SIZE 4096

#define SPIN_QUEUE_SIZE 2
#define SPIN_FRAME_SIZE 255

#include "espeak.h"
#include "text.h"

typedef enum
{
    IN      = 1,
    PROCESS = 2,
    OUT     = 4,
    PLAY    = 8
} SpinState;

typedef enum
{
    INPROCESS = 1
} ContextState;

typedef struct
{
    volatile SpinState state;

    Text text;
    goffset last_word;

    GMemoryOutputStream *sound;
    goffset sound_offset;

    GArray *events;
    goffset events_pos;
} Espin;

struct _Econtext
{
    volatile ContextState state;

    Espin queue[SPIN_QUEUE_SIZE];
    Espin *in;
    Espin *process;
    Espin *out;

    GSList *in_queue;
    GSList *process_chunk;

    volatile gint rate;
    volatile gint pitch;
    volatile const gchar *voice;

    GstElement *emitter;
    GstBus *bus;
};

static inline void
spinning(Espin *base, Espin **i)
{
    if (++(*i) == base + SPIN_QUEUE_SIZE)
        *i = base;
}

static void
emit_word(Econtext *self, guint offset, guint len)
{
    GstStructure *data = gst_structure_new("word",
            "offset", G_TYPE_UINT, offset,
            "len", G_TYPE_UINT, len,
            NULL);
    if (!self->bus)
        self->bus = gst_element_get_bus(self->emitter);
    GstMessage *msg = gst_message_new_element(GST_OBJECT(self->emitter), data);
    gst_bus_post(self->bus, msg);
}

static void init();
static void process_push(Econtext*);
static void process_pop(Econtext*);

static GThread *process_tid = NULL;
static GMutex *process_lock = NULL;
static GCond  *process_cond = NULL;
static GSList *process_queue = NULL;

static gint espeak_sample_rate = 0;
static const espeak_VOICE **espeak_voices = NULL;
static GOutputStream *espeak_buffer = NULL;
static GArray *espeak_events = NULL;

// -----------------------------------------------------------------------------

Econtext*
espeak_new(GstElement *emitter)
{
    init();

    Econtext *self = g_new0(Econtext, 1);
    gint i;

    for (i = SPIN_QUEUE_SIZE; i--;)
    {
        self->queue[i].state = IN;
        self->queue[i].sound = G_MEMORY_OUTPUT_STREAM(
                g_memory_output_stream_new(NULL, 0, realloc, free));
        self->queue[i].events = g_array_new(FALSE, FALSE, sizeof(espeak_EVENT));
    }

    self->in = self->queue;
    self->process = self->queue;
    self->out = self->queue;

    self->process_chunk = g_slist_alloc();
    self->process_chunk->data = self;

    self->pitch = ESPEAK_DEFAULT_PITCH;
    self->rate = ESPEAK_DEFAULT_RATE;
    self->voice = ESPEAK_DEFAULT_VOICE;

    self->emitter = emitter;
    gst_object_ref(self->emitter);

    self->bus = NULL;

    GST_DEBUG("[%p]", self);

    return self;
}

void
espeak_unref(Econtext *self)
{
    GST_DEBUG("[%p]", self);

    process_pop(self);

    gint i;

    for (i = SPIN_QUEUE_SIZE; i--;)
    {
        g_output_stream_close(G_OUTPUT_STREAM(self->queue[i].sound),
                NULL, NULL);
        g_object_unref(self->queue[i].sound);
        text_unref(&self->queue[i].text);
        g_array_free(self->queue[i].events, TRUE);
    }

    if (self->in_queue)
    {
        GSList *i;
        for (i = self->in_queue; i; i = g_slist_next(i))
            text_unref(i->data);
        g_slist_free(self->in_queue);
    }

    g_slist_free(self->process_chunk);

    gst_object_unref(self->bus);
    gst_object_unref(self->emitter);

    memset(self, 0, sizeof(Econtext));
    g_free(self);
}

// in/out ----------------------------------------------------------------------

static void
in_spinning(Econtext *self, Text *text)
{
    GST_DEBUG("[%p] text.body=%s text.offset=%ld text.frame_len=%ld",
            self, text->body, text->offset, text->frame_len);

    gboolean chunked = FALSE;

    while (!text_eot(text) && g_atomic_int_get(&self->in->state) == IN)
    {
        Espin *spin = self->in;
        text_chunk(text, &spin->text, SPIN_FRAME_SIZE);
        g_atomic_int_set(&spin->state, PROCESS);
        spinning(self->queue, &self->in);
        chunked = TRUE;
    }

    if (chunked)
        process_push(self);

    GST_DEBUG("[%p] text.body=%s text.offset=%ld text.frame_len=%ld",
            self, text->body, text->offset, text->frame_len);
}

void
espeak_in(Econtext *self, const gchar *str_)
{
    GST_DEBUG("[%p] str=%s", self, str_);

    if (str_ == NULL || *str_ == 0)
        return;

    Text *text = text_new(str_);

    if (self->in_queue)
    {
        self->in_queue = g_slist_append(self->in_queue, text);
        return;
    }

    in_spinning(self, text);

    if (!text_eot(text))
    {
        GST_DEBUG("[%p] text_len=%d", self, text_len(text));
        self->in_queue = g_slist_append(self->in_queue, text);
    }
}

GstBuffer*
play(Econtext *self, Espin *spin, gsize size_to_play)
{
    inline gsize whole(Espin *spin, gsize size_to_play)
    {
        gsize spin_size = g_memory_output_stream_get_data_size(spin->sound);
        return MIN(size_to_play, spin_size);
    }

    inline gsize words(Econtext *self, Espin *spin, gsize size_to_play)
    {
        gsize spin_size = g_memory_output_stream_get_data_size(spin->sound);
        size_to_play = MIN(size_to_play, spin_size);

        GST_DEBUG("spin_size=%ld size_to_play=%ld spin->events_pos=%ld",
                spin_size, size_to_play, spin->events_pos);

        goffset event;
        goffset sample_offset = 0;
        goffset text_offset = -1;
        gsize text_len = 0;

        for (event = spin->events_pos; TRUE; ++event)
        {
            espeak_EVENT *i = &g_array_index(spin->events, espeak_EVENT, event);

            if (i->type == espeakEVENT_LIST_TERMINATED)
            {
                GST_DEBUG("i->sample=%d", i->sample*2);
                sample_offset = spin_size;
                break;
            }
            else if (i->type == espeakEVENT_WORD)
            {
                sample_offset = i[1].sample*2;
                text_offset = spin->text.offset + i->text_position - 1;
                text_len = i->length;

                GST_DEBUG("sample_offset=%d txt_offset=%d txt_len=%d, txt=%s",
                        sample_offset, text_offset, text_len,
                        spin->text.body + text_offset);
                break;
            }
        }

        if (text_offset != -1 && text_offset > spin->last_word)
        {
            spin->last_word = text_offset + text_len;
            emit_word(self, text_offset, text_len);
        }

        if (sample_offset - spin->sound_offset > size_to_play)
        {
            GST_DEBUG("sample_offset=%ld spin->sound_offset=%ld",
                    sample_offset, spin->sound_offset);
            return size_to_play;
        }

        if (text_offset != -1)
            spin->events_pos = event + 1;

        return sample_offset - spin->sound_offset;
    }

    g_atomic_int_set(&spin->state, PLAY);

    if (self->emitter)
        size_to_play = words(self, spin, size_to_play);
    else
        size_to_play = whole(spin, size_to_play);

    GstBuffer *out = gst_buffer_new();
    GST_BUFFER_DATA(out) =
            (guchar*)g_memory_output_stream_get_data(spin->sound) +
                spin->sound_offset;
    GST_BUFFER_SIZE(out) = size_to_play;

    spin->sound_offset += size_to_play;

    GST_DEBUG("size_to_play=%ld tell=%ld", size_to_play, spin->sound_offset);

    return out;
}

GstBuffer*
espeak_out(Econtext *self, gsize size_to_play)
{
    GST_DEBUG("[%p] size_to_play=%d", self, size_to_play);

    for (;;)
    {
        g_mutex_lock(process_lock);
            if ((g_atomic_int_get(&self->out->state) & (PLAY|OUT)) == 0)
            {
                if (g_atomic_int_get(&self->state) & INPROCESS) 
                {
                    GST_DEBUG("[%p]", self);
                    g_cond_wait(process_cond, process_lock);
                }
                else
                {
                    GST_DEBUG("[%p]", self);
                    g_mutex_unlock(process_lock);
                    return NULL;
                }
            }
        g_mutex_unlock(process_lock);

        Espin *spin = self->out;
        gsize spin_size = g_memory_output_stream_get_data_size(spin->sound);

        GST_DEBUG("[%p] spin->sound_offset=%ld spin_size=%ld", self,
                spin->sound_offset, spin_size);

        if (g_atomic_int_get(&spin->state) == PLAY &&
                spin->sound_offset >= spin_size)
        {
            g_atomic_int_set(&spin->state, IN);
            text_unref(&spin->text);
            spinning(self->queue, &self->out);

            if (self->in_queue)
            {
                Text *text = self->in_queue->data;
                in_spinning(self, text);

                if (text_eot(text))
                {
                    self->in_queue = g_slist_delete_link(self->in_queue,
                            self->in_queue);
                    GST_DEBUG("[%p] in_queue=%d", self,
                            g_slist_length(self->in_queue));
                }
            }

            GST_DEBUG("[%p]", self);

            continue;
        }

        return play(self, spin, size_to_play);
    }

    return NULL;
}

// espeak ----------------------------------------------------------------------

static gint
synth_cb(short *data, int numsamples, espeak_EVENT *events)
{
    if (data == NULL)
        return 0;

    if (numsamples > 0)
    {
        g_output_stream_write(espeak_buffer, data, numsamples*2, NULL, NULL);

        for (; events->type != espeakEVENT_LIST_TERMINATED; ++events)
        {
            GST_DEBUG("type=%d text_position=%d length=%d "
                      "audio_position=%d sample=%d",
                    events->type, events->text_position, events->length,
                    events->audio_position, events->sample*2);
            g_array_append_val(espeak_events, *events);
        }
    }

    GST_DEBUG("numsamples=%d data_size=%ld", numsamples*2,
            g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(
                    espeak_buffer)));

    return 0;
}

static void
synth(Econtext *self, Espin *spin)
{
    gchar *text = text_first(&spin->text);
    gchar *last = text_last(&spin->text);

    gchar old_last_char = *last;
    *last = 0;

    GST_DEBUG("[%p] text='%s' last=%d", self, text, last-text);

    g_seekable_seek(G_SEEKABLE(spin->sound), 0, G_SEEK_SET,
            NULL, NULL);
    g_array_set_size(spin->events, 0);
    spin->sound_offset = 0;
    spin->events_pos = 0;
    spin->last_word = -1;

    espeak_SetParameter(espeakPITCH, g_atomic_int_get(&self->pitch), 0);
    espeak_SetParameter(espeakRATE, g_atomic_int_get(&self->rate), 0);
    espeak_SetVoiceByName((gchar*)g_atomic_pointer_get(&self->voice));
    espeak_buffer = G_OUTPUT_STREAM(spin->sound);
    espeak_events = spin->events;

    espeak_Synth(text, text_len(&spin->text), 0, POS_WORD, 0,
            espeakCHARS_UTF8|espeakPHONEMES, NULL, NULL);

    espeak_EVENT last_event = { espeakEVENT_LIST_TERMINATED };
    last_event.sample = g_memory_output_stream_get_data_size(spin->sound) / 2;
    g_array_append_val(spin->events, last_event);
    *last = old_last_char;
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
espeak_set_pitch(Econtext *self, guint value)
{
    g_atomic_int_set(&self->pitch, value);
}

void
espeak_set_rate(Econtext *self, guint value)
{
    g_atomic_int_set(&self->rate, value);
}

void
espeak_set_voice(Econtext *self, const gchar *value)
{
    g_atomic_pointer_set(&self->voice, value);
}

// process ----------------------------------------------------------------------

static gpointer
process(gpointer data)
{
    g_mutex_lock(process_lock);

    for (;;)
    {
        while (process_queue == NULL)
            g_cond_wait(process_cond, process_lock);

        while (process_queue)
        {
            Econtext *context = (Econtext*)process_queue->data;
            Espin *spin = context->process;

            process_queue = g_slist_remove_link(process_queue, process_queue);

            synth(context, spin);

            g_atomic_int_set(&spin->state, OUT);
            spinning(context->queue, &context->process);

            if (g_atomic_int_get(&context->process->state) == PROCESS)
            {
                GST_DEBUG("[%p]", context);
                process_queue = g_slist_concat(process_queue,
                        context->process_chunk);
            }
            else
            {
                GST_DEBUG("[%p]", context);
                g_atomic_int_set(&context->state,
                        g_atomic_int_get(&context->state) & ~INPROCESS);
            }
        }

        g_cond_broadcast(process_cond);
    }

    g_mutex_unlock(process_lock);

    return NULL;
}

static void
process_push(Econtext *context)
{
    GST_DEBUG("[%p]", context);
    g_mutex_lock(process_lock);

    int self_status = g_atomic_int_get(&context->state);

    if ((self_status & INPROCESS) == 0)
    {
        g_atomic_int_set(&context->state, self_status | INPROCESS);
        process_queue = g_slist_concat(process_queue, context->process_chunk);
        g_cond_broadcast(process_cond);
    }

    g_mutex_unlock(process_lock);
    GST_DEBUG("[%p]", context);
}

static void
process_pop(Econtext *context)
{
    GST_DEBUG("[%p]", context);
    g_mutex_lock(process_lock);

    process_queue = g_slist_remove_link(process_queue, context->process_chunk);
    g_cond_broadcast(process_cond);

    g_mutex_unlock(process_lock);
    GST_DEBUG("[%p]", context);
}

// -----------------------------------------------------------------------------

static void
init()
{
    static volatile gsize initialized = 0;

    if (initialized == 0)
    {
        ++initialized;
        espeak_sample_rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS,
                SYNC_BUFFER_SIZE, NULL, 0);
        espeak_SetSynthCallback(synth_cb);
        espeak_voices = espeak_ListVoices(NULL);

        process_lock = g_mutex_new();
        process_cond = g_cond_new();
        process_tid = g_thread_create(process, NULL, FALSE, NULL);
    }
}
