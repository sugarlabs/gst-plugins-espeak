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

#define SYNC_BUFFER_SIZE_MS 200
#define BYTES_PER_SAMPLE 2

#define SPIN_QUEUE_SIZE 2
#define SPIN_FRAME_SIZE 255

#include "espeak.h"

typedef enum {
    IN = 1,
    OUT = 2,
    PLAY = 4
} SpinState;

typedef enum {
    INPROCESS = 1,
    CLOSE = 2
} ContextState;

typedef struct {
    Econtext *context;

    volatile SpinState state;

    GByteArray *sound;
    gsize sound_offset;
    GstClockTime audio_position;

    GArray *events;
    gsize events_pos;

    int last_word;
    int mark_offset;
    const gchar *mark_name;
} Espin;

struct _Econtext {
    volatile ContextState state;

    gchar *text;
    gsize text_offset;
    gsize text_len;
    gchar *next_mark;

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

static inline void spinning (Espin * base, Espin ** i) {
    if (++(*i) == base + SPIN_QUEUE_SIZE)
        *i = base;
}

static void post_message (Econtext * self, GstStructure * data) {
    if (!self->bus)
        self->bus = gst_element_get_bus (self->emitter);
    GstMessage *msg =
            gst_message_new_element (GST_OBJECT (self->emitter), data);
    gst_bus_post (self->bus, msg);
}

static void emit_word (Econtext * self, guint offset, guint len, guint id) {
    post_message (self, gst_structure_new ("espeak-word",
                    "offset", G_TYPE_UINT, offset,
                    "len", G_TYPE_UINT, len, "id", G_TYPE_UINT, id, NULL));
}

static void emit_sentence (Econtext * self, guint offset, guint len, guint id) {
    post_message (self, gst_structure_new ("espeak-sentence",
                    "offset", G_TYPE_UINT, offset,
                    "len", G_TYPE_UINT, len, "id", G_TYPE_UINT, id, NULL));
}

static void emit_mark (Econtext * self, guint offset, const gchar * mark) {
    post_message (self, gst_structure_new ("espeak-mark",
                    "offset", G_TYPE_UINT, offset,
                    "mark", G_TYPE_STRING, mark, NULL));
}

static void init ();
static void process_push (Econtext *, gboolean);
static void process_pop (Econtext *);

static GThread *process_tid = NULL;
static GMutex *process_lock = NULL;
static GCond *process_cond = NULL;
static GSList *process_queue = NULL;

static gint espeak_sample_rate = 0;
static gint espeak_buffer_size = 0;
static GValueArray *espeak_voices = NULL;

// -----------------------------------------------------------------------------

Econtext *espeak_new (GstElement * emitter) {
    init ();

    Econtext *self = g_new0 (Econtext, 1);
    gint i;

    for (i = SPIN_QUEUE_SIZE; i--;) {
        Espin *spin = &self->queue[i];

        spin->context = self;
        spin->state = IN;
        spin->sound = g_byte_array_new ();
        spin->events = g_array_new (FALSE, FALSE, sizeof (espeak_EVENT));
    }

    self->in = self->queue;
    self->out = self->queue;

    self->process_chunk = g_slist_alloc ();
    self->process_chunk->data = self;

    self->pitch = 50;
    self->rate = 170;
    self->voice = ESPEAK_DEFAULT_VOICE;
    self->gap = 0;
    self->track = ESPEAK_TRACK_NONE;

    self->emitter = emitter;
    gst_object_ref (self->emitter);
    self->bus = NULL;

    GST_DEBUG ("[%p]", self);

    return self;
}

void espeak_unref (Econtext * self) {
    GST_DEBUG ("[%p]", self);

    espeak_reset (self);

    gint i;

    for (i = SPIN_QUEUE_SIZE; i--;) {
        g_byte_array_free (self->queue[i].sound, TRUE);
        g_array_free (self->queue[i].events, TRUE);
    }

    g_slist_free (self->process_chunk);

    gst_object_unref (self->bus);
    gst_object_unref (self->emitter);

    memset (self, 0, sizeof (Econtext));
    g_free (self);
}

// in/out ----------------------------------------------------------------------

void espeak_in (Econtext * self, const gchar * text) {
    GST_DEBUG ("[%p] text=%s", self, text);

    if (text == NULL || *text == 0)
        return;

    self->text = g_strdup (text);
    self->text_offset = 0;
    self->text_len = strlen (text);

    process_push (self, TRUE);
}

GstBuffer *play (Econtext * self, Espin * spin, gsize size_to_play) {
    inline gsize whole (Espin * spin, gsize size_to_play) {
        for (;; ++spin->events_pos) {
            espeak_EVENT *i = &g_array_index (spin->events, espeak_EVENT,
                    spin->events_pos);
            gsize len = i->sample * BYTES_PER_SAMPLE - spin->sound_offset;

            if (i->type == espeakEVENT_LIST_TERMINATED || len >= size_to_play)
                return len;
        }
    }

    inline gsize events (Econtext * self, Espin * spin, gsize size_to_play) {
        gsize spin_size = spin->sound->len;
        gsize event;
        gsize sample_offset = 0;
        espeak_EVENT *i =
                &g_array_index (spin->events, espeak_EVENT, spin->events_pos);

        GST_DEBUG ("event=%zd i->type=%d i->text_position=%d",
                event, i->type, i->text_position);

        if (i->type == espeakEVENT_LIST_TERMINATED) {
            sample_offset = spin_size;
        } else {
            switch (i->type) {
            case espeakEVENT_MARK:
                emit_mark (self, i->text_position, i->id.name);
                break;
            case espeakEVENT_WORD:
                emit_word (self, i->text_position, i->length, i->id.number);
                break;
            case espeakEVENT_SENTENCE:
                emit_sentence (self, i->text_position, i->length, i->id.number);
                break;
            }
        }

        if (!sample_offset) {
            sample_offset = i->sample * BYTES_PER_SAMPLE;
        }

        return sample_offset - spin->sound_offset;
    }

    g_atomic_int_set (&spin->state, PLAY);

    switch (g_atomic_int_get (&self->track)) {
    case ESPEAK_TRACK_WORD:
    case ESPEAK_TRACK_MARK:
        size_to_play = events (self, spin, size_to_play);
        break;
    default:
        size_to_play = whole (spin, size_to_play);
        break;
    }

    espeak_EVENT *event = &g_array_index (spin->events, espeak_EVENT,
            spin->events_pos);

    GstBuffer *out = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY |
            GST_MEMORY_FLAG_NO_SHARE,
            spin->sound->data, spin->sound->len,
            spin->sound_offset, size_to_play, NULL, NULL);

    GST_BUFFER_OFFSET (out) = spin->sound_offset;
    GST_BUFFER_OFFSET_END (out) = spin->sound_offset + size_to_play;
    GST_BUFFER_TIMESTAMP (out) = spin->audio_position;
    spin->audio_position =
            gst_util_uint64_scale_int (event->audio_position, GST_SECOND, 1000);
    GST_BUFFER_DURATION (out) =
            spin->audio_position - GST_BUFFER_TIMESTAMP (out);

    spin->sound_offset += size_to_play;
    spin->events_pos += 1;

    GST_DEBUG ("size_to_play=%zd tell=%zd ts=%" G_GUINT64_FORMAT " dur=%"
            G_GUINT64_FORMAT, size_to_play,
            spin->sound_offset, GST_BUFFER_TIMESTAMP (out),
            GST_BUFFER_DURATION (out));

    return out;
}

GstBuffer *espeak_out (Econtext * self, gsize size_to_play) {
    GST_DEBUG ("[%p] size_to_play=%d", self, size_to_play);

    for (;;) {
        g_mutex_lock (process_lock);
        for (;;) {
            if (g_atomic_int_get (&self->out->state) & (PLAY | OUT))
                break;
            if (self->state != INPROCESS) {
                if (self->state == CLOSE)
                    GST_DEBUG ("[%p] sesseion is closed", self);
                else
                    GST_DEBUG ("[%p] nothing to play", self);
                g_mutex_unlock (process_lock);
                return NULL;
            }
            GST_DEBUG ("[%p] wait for processed data", self);
            g_cond_wait (process_cond, process_lock);
        }
        g_mutex_unlock (process_lock);

        Espin *spin = self->out;
        gsize spin_size = spin->sound->len;

        GST_DEBUG ("[%p] spin=%p spin->sound_offset=%zd spin_size=%zd "
                "spin->state=%d",
                self, spin, spin->sound_offset, spin_size,
                g_atomic_int_get (&spin->state));

        if (g_atomic_int_get (&spin->state) == PLAY &&
                spin->sound_offset >= spin_size) {
            g_atomic_int_set (&spin->state, IN);
            process_push (self, FALSE);
            spinning (self->queue, &self->out);
            continue;
        }

        return play (self, spin, size_to_play);
    }

    GST_DEBUG ("[%p]", self);
    return NULL;
}

void espeak_reset (Econtext * self) {
    process_pop (self);

    GstBuffer *buf;
    while ((buf = espeak_out (self, espeak_buffer_size)) != NULL)
        gst_buffer_unref (buf);

    int i;
    for (i = SPIN_QUEUE_SIZE; i--;)
        g_atomic_int_set (&self->queue[i].state, IN);

    if (self->text) {
        g_free (self->text);
        self->text = NULL;
    }

    self->next_mark = NULL;
}

// espeak ----------------------------------------------------------------------

static gint synth_cb (short *data, int numsamples, espeak_EVENT * events) {
    if (data == NULL)
        return 0;

    Espin *spin = events->user_data;
    Econtext *self = spin->context;

    if (numsamples > 0) {
        g_byte_array_append (spin->sound, (const guint8 *) data,
                numsamples * BYTES_PER_SAMPLE);

        espeak_EVENT *i;

        for (i = events; i->type != espeakEVENT_LIST_TERMINATED; ++i) {
            GST_DEBUG ("type=%d text_position=%d length=%d "
                    "audio_position=%d sample=%d",
                    i->type, i->text_position, i->length,
                    i->audio_position, i->sample * BYTES_PER_SAMPLE);

            // convert to 0-based position
            --i->text_position;

            if (i->type == espeakEVENT_MARK) {
                // point mark name to our text substring instead of
                // one which was temporally allocated by espeak
                if (self->next_mark == NULL)
                    self->next_mark = self->text;
                int mark_len = strlen (i->id.name);
                strncpy (self->next_mark, i->id.name, mark_len);
                i->id.name = self->next_mark;
                self->next_mark[mark_len] = '\0';
                self->next_mark += mark_len + 1;
            }

            GST_DEBUG ("text_position=%d length=%d",
                    i->text_position, i->length);

            g_array_append_val (spin->events, *i);
        }
    }

    GST_DEBUG ("numsamples=%d", numsamples * BYTES_PER_SAMPLE);

    return 0;
}

static void synth (Econtext * self, Espin * spin) {
    g_byte_array_set_size (spin->sound, 0);
    g_array_set_size (spin->events, 0);
    spin->sound_offset = 0;
    spin->audio_position = 0;
    spin->events_pos = 0;
    spin->mark_offset = 0;
    spin->mark_name = NULL;
    spin->last_word = -1;

    espeak_SetParameter (espeakPITCH, g_atomic_int_get (&self->pitch), 0);
    espeak_SetParameter (espeakRATE, g_atomic_int_get (&self->rate), 0);
    espeak_SetVoiceByName ((gchar *) g_atomic_pointer_get (&self->voice));
    espeak_SetParameter (espeakWORDGAP, g_atomic_int_get (&self->gap), 0);

    gint track = g_atomic_int_get (&self->track);

    gint flags = espeakCHARS_UTF8;
    if (track == ESPEAK_TRACK_MARK)
        flags |= espeakSSML;

    GST_DEBUG ("[%p] text_offset=%zd", self, self->text_offset);

    espeak_Synth (self->text, self->text_len + 1, 0, POS_CHARACTER, 0, flags,
            NULL, spin);

    if (spin->events->len) {
        int text_offset = g_array_index (spin->events, espeak_EVENT,
                spin->events->len - 1).text_position + 1;
        self->text_offset = g_utf8_offset_to_pointer (self->text, text_offset)
                - self->text;
    }

    espeak_EVENT last_event = { espeakEVENT_LIST_TERMINATED };
    last_event.sample = spin->sound->len / BYTES_PER_SAMPLE;
    g_array_append_val (spin->events, last_event);
}

gint espeak_get_sample_rate () {
    return espeak_sample_rate;
}

gint espeak_get_buffer_size () {
    return espeak_buffer_size;
}

GValueArray *espeak_get_voices () {
    init ();
    return g_value_array_copy (espeak_voices);
}

void espeak_set_pitch (Econtext * self, gint value) {
    if (value == 0)
        value = 50;
    else
        value = MIN (99, (value + 100) / 2);

    g_atomic_int_set (&self->pitch, value);
}

void espeak_set_rate (Econtext * self, gint value) {
    if (value == 0)
        value = 170;
    else if (value < 0)
        value = MAX (80, value + 170);
    else
        value = 170 + value * 2;

    g_atomic_int_set (&self->rate, value);
}

void espeak_set_voice (Econtext * self, const gchar * value) {
    g_atomic_pointer_set (&self->voice, value);
}

void espeak_set_gap (Econtext * self, guint value) {
    g_atomic_int_set (&self->gap, value);
}

void espeak_set_track (Econtext * self, guint value) {
    g_atomic_int_set (&self->track, value);
}

// process ----------------------------------------------------------------------

static gpointer process (gpointer data) {
    g_mutex_lock (process_lock);

    for (;;) {
        while (process_queue == NULL)
            g_cond_wait (process_cond, process_lock);

        while (process_queue) {
            Econtext *context = (Econtext *) process_queue->data;
            Espin *spin = context->in;

            process_queue = g_slist_remove_link (process_queue, process_queue);

            if (context->state == CLOSE) {
                GST_DEBUG ("[%p] session is closed", context);
                continue;
            }

            GST_DEBUG ("[%p] context->text_offset=%d context->text_len=%d",
                    context, context->text_offset, context->text_len);

            if (context->text_offset >= context->text_len) {
                GST_DEBUG ("[%p] end of text to process", context);
                context->state &= ~INPROCESS;
            } else {
                synth (context, spin);
                g_atomic_int_set (&spin->state, OUT);
                spinning (context->queue, &context->in);

                if (g_atomic_int_get (&context->in->state) == IN) {
                    GST_DEBUG ("[%p] continue to process data", context);
                    process_queue = g_slist_concat (process_queue,
                            context->process_chunk);
                } else {
                    GST_DEBUG ("[%p] pause to process data", context);
                    context->state &= ~INPROCESS;
                }
            }
        }

        g_cond_broadcast (process_cond);
    }

    g_mutex_unlock (process_lock);

    return NULL;
}

static void process_push (Econtext * context, gboolean force_in) {
    GST_DEBUG ("[%p] lock", context);
    g_mutex_lock (process_lock);

    if (context->state == CLOSE && !force_in)
        GST_DEBUG ("[%p] state=%d", context, context->state);
    else if (context->state != INPROCESS) {
        context->state = INPROCESS;
        process_queue = g_slist_concat (process_queue, context->process_chunk);
        g_cond_broadcast (process_cond);
    }

    g_mutex_unlock (process_lock);
    GST_DEBUG ("[%p] unlock", context);
}

static void process_pop (Econtext * context) {
    GST_DEBUG ("[%p] lock", context);
    g_mutex_lock (process_lock);

    process_queue = g_slist_remove_link (process_queue, context->process_chunk);
    context->state = CLOSE;
    g_cond_broadcast (process_cond);

    g_mutex_unlock (process_lock);
    GST_DEBUG ("[%p] unlock", context);
}

// -----------------------------------------------------------------------------

static void init () {
    static volatile gsize initialized = 0;

    if (initialized == 0) {
        ++initialized;

        process_lock = g_mutex_new ();
        process_cond = g_cond_new ();
        process_tid = g_thread_create (process, NULL, FALSE, NULL);

        espeak_sample_rate = espeak_Initialize (AUDIO_OUTPUT_SYNCHRONOUS,
                SYNC_BUFFER_SIZE_MS, NULL, 0);
        espeak_buffer_size =
                (SYNC_BUFFER_SIZE_MS * espeak_sample_rate) /
                1000 / BYTES_PER_SAMPLE;
        espeak_SetSynthCallback (synth_cb);

        gsize count = 0;
        const espeak_VOICE **i;
        const espeak_VOICE **voices = espeak_ListVoices (NULL);

        for (i = voices; *i; ++i)
            ++count;
        espeak_voices = g_value_array_new (count);

        for (i = voices; *i; ++i) {
            GValueArray *voice = g_value_array_new (2);

            GValue name = { 0 };
            g_value_init (&name, G_TYPE_STRING);
            g_value_set_static_string (&name, (*i)->name);
            g_value_array_append (voice, &name);

            char *dialect_str = strchr ((*i)->languages + 1, '-');
            if (dialect_str)
                *dialect_str++ = 0;

            GValue lang = { 0 };
            g_value_init (&lang, G_TYPE_STRING);
            g_value_set_static_string (&lang, (*i)->languages + 1);
            g_value_array_append (voice, &lang);

            GValue dialect = { 0 };
            g_value_init (&dialect, G_TYPE_STRING);
            g_value_set_static_string (&dialect,
                    dialect_str ? dialect_str : "none");
            g_value_array_append (voice, &dialect);

            GValue voice_value = { 0 };
            g_value_init (&voice_value, G_TYPE_VALUE_ARRAY);
            g_value_take_boxed (&voice_value, voice);
            g_value_array_append (espeak_voices, &voice_value);
            g_value_unset (&voice_value);
        }
    }
}
