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

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gst/gst.h>
#include <espeak/speak_lib.h>

#define SYNC_BUFFER_SIZE 4096

#define SPIN_QUEUE_SIZE 2
#define SPIN_FRAME_SIZE 255

#include "espeak.h"

typedef enum
{
    IN      = 1,
    OUT     = 2,
    PLAY    = 4
} SpinState;

typedef enum
{
    INPROCESS = 1,
    CLOSE     = 2
} ContextState;

typedef struct
{
    Econtext *context;

    volatile SpinState state;

    GByteArray *sound;
    gsize sound_offset;

    GArray *events;
    gsize events_pos;

    gsize last_word;
    gsize mark_offset;
    const gchar *mark_name;
    gsize last_mark;
} Espin;

struct _Econtext
{
    volatile ContextState state;

    gchar *text;
    gsize text_offset;
    gsize text_len;

    Espin queue[SPIN_QUEUE_SIZE];
    Espin *in;
    Espin *out;

    GSList *process_chunk;

    volatile gint rate;
    volatile gint pitch;
    volatile const gchar *voice;
    volatile gint gap;
    volatile gint track;

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
    GstStructure *data = gst_structure_new("espeak-word",
            "offset", G_TYPE_UINT, offset,
            "len", G_TYPE_UINT, len,
            NULL);
    if (!self->bus)
        self->bus = gst_element_get_bus(self->emitter);
    GstMessage *msg = gst_message_new_element(GST_OBJECT(self->emitter), data);
    gst_bus_post(self->bus, msg);
}

static void
emit_mark(Econtext *self, guint offset, const gchar *mark)
{
    GstStructure *data = gst_structure_new("espeak-mark",
            "offset", G_TYPE_UINT, offset,
            "mark", G_TYPE_STRING, mark,
            NULL);
    if (!self->bus)
        self->bus = gst_element_get_bus(self->emitter);
    GstMessage *msg = gst_message_new_element(GST_OBJECT(self->emitter), data);
    gst_bus_post(self->bus, msg);
}

static inline gsize
strbstr(Econtext *self, gsize pos, const gchar *needle, gsize needle_len)
{
    for (pos -= needle_len; pos >= needle_len; --pos)
        if (strncmp(self->text + pos, needle, needle_len) == 0)
            return pos;
    return 0;
}

static void init();
static void process_push(Econtext*, gboolean);
static void process_pop(Econtext*);

static GThread *process_tid = NULL;
static GMutex *process_lock = NULL;
static GCond  *process_cond = NULL;
static GSList *process_queue = NULL;

static gint espeak_sample_rate = 0;
static GValueArray *espeak_voices = NULL;

// -----------------------------------------------------------------------------

Econtext*
espeak_new(GstElement *emitter)
{
    init();

    Econtext *self = g_new0(Econtext, 1);
    gint i;

    for (i = SPIN_QUEUE_SIZE; i--;)
    {
        Espin *spin = &self->queue[i];

        spin->context = self;
        spin->state = IN;
        spin->sound = g_byte_array_new();
        spin->events = g_array_new(FALSE, FALSE, sizeof(espeak_EVENT));
    }

    self->in = self->queue;
    self->out = self->queue;

    self->process_chunk = g_slist_alloc();
    self->process_chunk->data = self;

    self->pitch = 50;
    self->rate = 170;
    self->voice = ESPEAK_DEFAULT_VOICE;
    self->gap = 0;
    self->track = ESPEAK_TRACK_NONE;

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

    espeak_reset(self);

    gint i;

    for (i = SPIN_QUEUE_SIZE; i--;)
    {
        g_byte_array_free(self->queue[i].sound, TRUE);
        g_array_free(self->queue[i].events, TRUE);
    }

    g_slist_free(self->process_chunk);

    gst_object_unref(self->bus);
    gst_object_unref(self->emitter);

    memset(self, 0, sizeof(Econtext));
    g_free(self);
}

// in/out ----------------------------------------------------------------------

void
espeak_in(Econtext *self, const gchar *text)
{
    GST_DEBUG("[%p] text=%s", self, text);

    if (text == NULL || *text == 0)
        return;

    self->text = g_strdup(text);
    self->text_offset = 0;
    self->text_len = strlen(text);

    process_push(self, TRUE);
}

GstBuffer*
play(Econtext *self, Espin *spin, gsize size_to_play)
{
    inline gsize whole(Espin *spin, gsize size_to_play)
    {
        gsize spin_size = spin->sound->len;
        return MIN(size_to_play, spin_size - spin->sound_offset);
    }

    inline gsize word(Econtext *self, Espin *spin, gsize size_to_play)
    {
        gsize spin_size = spin->sound->len;
        size_to_play = MIN(size_to_play, spin_size);

        gsize event;
        gsize sample_offset = 0;
        gsize text_offset = -1;
        gsize text_len = 0;

        for (event = spin->events_pos; TRUE; ++event)
        {
            espeak_EVENT *i = &g_array_index(spin->events, espeak_EVENT, event);

            GST_DEBUG("size_to_play=%zd event=%zd "
                      "i->type=%d i->text_position=%d",
                      size_to_play, event, i->type, i->text_position);

            if (i->type == espeakEVENT_LIST_TERMINATED)
            {
                GST_DEBUG("i->sample=%d", i->sample*2);
                sample_offset = spin_size;
                break;
            }
            else if (i->type == espeakEVENT_WORD)
            {
                sample_offset = i[1].sample*2;
                text_offset = i->text_position;
                text_len = i->length;

                GST_DEBUG("sample_offset=%d txt_offset=%d txt_len=%d, txt=%s",
                        sample_offset, text_offset, text_len,
                        self->text + text_offset);
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
            GST_DEBUG("sample_offset=%zd spin->sound_offset=%zd",
                    sample_offset, spin->sound_offset);
            return size_to_play;
        }

        if (text_offset != -1)
            spin->events_pos = event + 1;

        return sample_offset - spin->sound_offset;
    }

    inline gsize mark(Econtext *self, Espin *spin, gsize size_to_play)
    {
        if (spin->mark_name)
        {
            emit_mark(self, spin->mark_offset, spin->mark_name);
            spin->mark_offset = -1;
            spin->mark_name = NULL;
        }

        gsize spin_size = spin->sound->len;
        size_to_play = MIN(size_to_play, spin_size);

        gsize event;
        gsize sample_offset = 0;
        guint mark_offset = 0;
        const gchar *mark_name = NULL;

        for (event = spin->events_pos; TRUE; ++event)
        {
            espeak_EVENT *i = &g_array_index(spin->events, espeak_EVENT, event);

            GST_DEBUG("size_to_play=%zd event=%zd "
                      "i->type=%d i->text_position=%d",
                      size_to_play, event, i->type, i->text_position);

            if (i->type == espeakEVENT_LIST_TERMINATED)
            {
                sample_offset = spin_size;
                break;
            }
            else if (i->type == espeakEVENT_MARK)
            {
                if (i->sample == 0)
                {
                    if (spin->sound_offset == 0)
                        emit_mark(self, i->text_position, i->id.name);
                    continue;
                }

                mark_offset = i->text_position;
                mark_name = i->id.name;
                sample_offset = i->sample*2;
                break;
            }
        }

        if (sample_offset - spin->sound_offset > size_to_play)
        {
            GST_DEBUG("sample_offset=%zd spin->sound_offset=%zd",
                    sample_offset, spin->sound_offset);
            return size_to_play;
        }

        spin->mark_offset = mark_offset;
        spin->mark_name = mark_name;
        spin->events_pos = event + 1;

        return sample_offset - spin->sound_offset;
    }

    g_atomic_int_set(&spin->state, PLAY);

    switch (g_atomic_int_get(&self->track))
    {
        case ESPEAK_TRACK_WORD:
            size_to_play = word(self, spin, size_to_play);
            break;
        case ESPEAK_TRACK_MARK:
            size_to_play = mark(self, spin, size_to_play);
            break;
        default:
            size_to_play = whole(spin, size_to_play);
    }

    GstBuffer *out = gst_buffer_new();
    GST_BUFFER_DATA(out) = spin->sound->data + spin->sound_offset;
    GST_BUFFER_SIZE(out) = size_to_play;

    spin->sound_offset += size_to_play;

    GST_DEBUG("out=%p size_to_play=%zd tell=%zd", GST_BUFFER_DATA(out),
            size_to_play, spin->sound_offset);

    return out;
}

GstBuffer*
espeak_out(Econtext *self, gsize size_to_play)
{
    GST_DEBUG("[%p] size_to_play=%d", self, size_to_play);

    for (;;)
    {
        g_mutex_lock(process_lock);
            for (;;)
            {
                if (g_atomic_int_get(&self->out->state) & (PLAY|OUT))
                    break;
                if (self->state != INPROCESS)
                {
                    if (self->state == CLOSE)
                        GST_DEBUG("[%p] sesseion is closed", self);
                    else
                        GST_DEBUG("[%p] nothing to play", self);
                    g_mutex_unlock(process_lock);
                    return NULL;
                }
                GST_DEBUG("[%p] wait for processed data", self);
                g_cond_wait(process_cond, process_lock);
            }
        g_mutex_unlock(process_lock);

        Espin *spin = self->out;
        gsize spin_size = spin->sound->len;

        GST_DEBUG("[%p] spin->sound_offset=%zd spin_size=%zd",
                self, spin->sound_offset, spin_size);

        if (g_atomic_int_get(&spin->state) == PLAY &&
                spin->sound_offset >= spin_size)
        {
            g_atomic_int_set(&spin->state, IN);
            process_push(self, FALSE);
            spinning(self->queue, &self->out);
            continue;
        }

        return play(self, spin, size_to_play);
    }

    GST_DEBUG("[%p]", self);
    return NULL;
}

void
espeak_reset(Econtext *self)
{
    process_pop(self);

    GstBuffer *buf;
    while ((buf = espeak_out(self, SYNC_BUFFER_SIZE)) != NULL)
        gst_buffer_unref(buf);

    int i;
    for (i = SPIN_QUEUE_SIZE; i--;)
        g_atomic_int_set(&self->queue[i].state, IN);

    if (self->text)
    {
        g_free(self->text);
        self->text = NULL;
    }
}

// espeak ----------------------------------------------------------------------

static gint
synth_cb(short *data, int numsamples, espeak_EVENT *events)
{
    if (data == NULL)
        return 0;

    Espin *spin = events->user_data;
    Econtext *self = spin->context;

    if (numsamples > 0)
    {
        g_byte_array_append(spin->sound, (const guint8*)data, numsamples*2);

        espeak_EVENT *i;

        for (i = events; i->type != espeakEVENT_LIST_TERMINATED; ++i)
        {
            // convert to 0-based position
            --i->text_position;

            GST_DEBUG("type=%d text_position=%d length=%d "
                      "audio_position=%d sample=%d",
                    i->type, i->text_position, i->length,
                    i->audio_position, i->sample*2);


            // workaround to fix text_position related faults for mark events
            if (i->type == espeakEVENT_MARK)
            {
                // suppress failed text_position values
                if (spin->last_mark)
                {
                    const gchar *eom = strstr(self->text +
                            spin->last_mark, "/>");
                    if (eom)
                    {
                        gsize pos = eom - self->text + 2;

                        if (i->text_position <= spin->last_mark ||
                                pos > i->text_position)
                            i->text_position = pos;
                    }
                }

                spin->last_mark = i->text_position;

                // point mark name to text substring instead of using
                // espeak's allocated string
                gsize l_quote, r_quote;
                if ((r_quote = strbstr(self, i->text_position, "/>", 2)) != 0)
                    if ((r_quote = strbstr(self, r_quote, "\"", 1)) != 0)
                        if ((l_quote = strbstr(self, r_quote, "\"", 1)) != 0)
                        {
                            i->id.name = self->text + l_quote + 1;
                            self->text[r_quote] = 0;
                        }

            }

            GST_DEBUG("text_position=%d length=%d",
                    i->text_position, i->length);

            g_array_append_val(spin->events, *i);
        }
    }

    GST_DEBUG("numsamples=%d", numsamples*2);

    return 0;
}

static void
synth(Econtext *self, Espin *spin)
{
    g_byte_array_set_size(spin->sound, 0);
    g_array_set_size(spin->events, 0);
    spin->sound_offset = 0;
    spin->events_pos = 0;
    spin->mark_offset = -1;
    spin->mark_name = NULL;
    spin->last_word = -1;
    spin->last_mark = 0;

    espeak_SetParameter(espeakPITCH, g_atomic_int_get(&self->pitch), 0);
    espeak_SetParameter(espeakRATE, g_atomic_int_get(&self->rate), 0);
    espeak_SetVoiceByName((gchar*)g_atomic_pointer_get(&self->voice));
    espeak_SetParameter(espeakWORDGAP, g_atomic_int_get(&self->gap), 0);

    gint track = g_atomic_int_get(&self->track);

    gint flags = espeakCHARS_UTF8;
    if (track == ESPEAK_TRACK_MARK)
        flags |= espeakSSML;

    GST_DEBUG("[%p] text_offset=%zd", self, self->text_offset);

    espeak_Synth(self->text, self->text_len + 1, 0, POS_CHARACTER, 0, flags,
            NULL, spin);

    if (spin->events->len)
    {
        self->text_offset = g_array_index(spin->events,
                espeak_EVENT, spin->events->len-1).text_position + 1;
    }

    espeak_EVENT last_event = { espeakEVENT_LIST_TERMINATED };
    last_event.sample = spin->sound->len / 2;
    g_array_append_val(spin->events, last_event);
}

gint
espeak_get_sample_rate()
{
    return espeak_sample_rate;
}

GValueArray*
espeak_get_voices()
{
    init();
    return g_value_array_copy(espeak_voices);
}

void
espeak_set_pitch(Econtext *self, gint value)
{
    if (value == 0)
        value = 50;
    else
        value = MIN(99, (value + 100) / 2);

    g_atomic_int_set(&self->pitch, value);
}

void
espeak_set_rate(Econtext *self, gint value)
{
    if (value == 0)
        value = 170;
    else
        if (value < 0)
            value = MAX(80, value + 170);
        else
            value = 170 + value * 2;

    g_atomic_int_set(&self->rate, value);
}

void
espeak_set_voice(Econtext *self, const gchar *value)
{
    g_atomic_pointer_set(&self->voice, value);
}

void
espeak_set_gap(Econtext *self, guint value)
{
    g_atomic_int_set(&self->gap, value);
}

void
espeak_set_track(Econtext *self, guint value)
{
    g_atomic_int_set(&self->track, value);
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
            Espin *spin = context->in;

            process_queue = g_slist_remove_link(process_queue, process_queue);

            if (context->state == CLOSE)
            {
                GST_DEBUG("[%p] session is closed", context);
                continue;
            }

            if (context->text_offset >= context->text_len)
            {
                GST_DEBUG("[%p] end of text to process", context);
                context->state &= ~INPROCESS;
            }
            else
            {
                synth(context, spin);
                g_atomic_int_set(&spin->state, OUT);
                spinning(context->queue, &context->in);

                if (g_atomic_int_get(&context->in->state) == IN)
                {
                    GST_DEBUG("[%p] continue to process data", context);
                    process_queue = g_slist_concat(process_queue,
                            context->process_chunk);
                }
                else
                {
                    GST_DEBUG("[%p] pause to process data", context);
                    context->state &= ~INPROCESS;
                }
            }
        }

        g_cond_broadcast(process_cond);
    }

    g_mutex_unlock(process_lock);

    return NULL;
}

static void
process_push(Econtext *context, gboolean force_in)
{
    GST_DEBUG("[%p] lock", context);
    g_mutex_lock(process_lock);

    if (context->state == CLOSE && !force_in)
        GST_DEBUG("[%p] state=%d", context, context->state);
    else if (context->state != INPROCESS)
    {
        context->state = INPROCESS;
        process_queue = g_slist_concat(process_queue, context->process_chunk);
        g_cond_broadcast(process_cond);
    }

    g_mutex_unlock(process_lock);
    GST_DEBUG("[%p] unlock", context);
}

static void
process_pop(Econtext *context)
{
    GST_DEBUG("[%p] lock", context);
    g_mutex_lock(process_lock);

    process_queue = g_slist_remove_link(process_queue, context->process_chunk);
    context->state = CLOSE;
    g_cond_broadcast(process_cond);

    g_mutex_unlock(process_lock);
    GST_DEBUG("[%p] unlock", context);
}

// -----------------------------------------------------------------------------

static void
init()
{
    static volatile gsize initialized = 0;

    if (initialized == 0)
    {
        ++initialized;

        process_lock = g_mutex_new();
        process_cond = g_cond_new();
        process_tid = g_thread_create(process, NULL, FALSE, NULL);

        espeak_sample_rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS,
                SYNC_BUFFER_SIZE, NULL, 0);
        espeak_SetSynthCallback(synth_cb);

        gsize count = 0;
        const espeak_VOICE **i;
        const espeak_VOICE **voices = espeak_ListVoices(NULL);

        for (i = voices; *i; ++i)
            ++count;
        espeak_voices = g_value_array_new(count);

        for (i = voices; *i; ++i)
        {
            GValueArray *voice = g_value_array_new(2);

            GValue name = { 0 };
            g_value_init(&name, G_TYPE_STRING);
            g_value_set_static_string(&name, (*i)->name);
            g_value_array_append(voice, &name);

            char *dialect_str = strchr((*i)->languages + 1, '-');
            if (dialect_str) *dialect_str++ = 0;

            GValue lang = { 0 };
            g_value_init(&lang, G_TYPE_STRING);
            g_value_set_static_string(&lang, (*i)->languages + 1);
            g_value_array_append(voice, &lang);

            GValue dialect = { 0 };
            g_value_init(&dialect, G_TYPE_STRING);
            g_value_set_static_string(&dialect, dialect_str ? dialect_str : "none");
            g_value_array_append(voice, &dialect);

            GValue voice_value = { 0 };
            g_value_init(&voice_value, G_TYPE_VALUE_ARRAY);
            g_value_set_boxed_take_ownership(&voice_value, voice);
            g_value_array_append(espeak_voices, &voice_value);
            g_value_unset(&voice_value);
        }
    }
}
