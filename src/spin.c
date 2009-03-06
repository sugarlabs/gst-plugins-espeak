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
#include <gst/gst.h>

#define SPIN_QUEUE_SIZE 2
#define SPIN_FRAME_SIZE 128

#include "text.h"

typedef void (*EspeakCallBack)(const gchar*, GMemoryOutputStream*, gpointer);

typedef enum
{
    IN      = 1,
    PROCESS = 2,
    OUT     = 4,
    PLAY    = 8
} SpinState;

typedef enum
{
    CLOSE     = 1,
    INPROCESS = 2
} ContextState;

typedef struct
{
    SpinState state;
    Text text;
    GMemoryOutputStream *sound;
    gsize sound_pos;
} Espin;

typedef struct 
{
    pthread_mutex_t lock;
    pthread_cond_t cond;
    ContextState state;

    Espin queue[SPIN_QUEUE_SIZE];
    Espin *in;
    Espin *process;
    Espin *out;

    GSList *in_queue;
    GSList *process_chunk;
    gpointer closure;
} Econtext;

static inline void
spinning(Espin *base, Espin **i)
{
    if (++(*i) == base + SPIN_QUEUE_SIZE)
        *i = base;
}

static void process_push(Econtext*);

// -----------------------------------------------------------------------------

Econtext*
spin_new(gpointer closure)
{
    Econtext *self = g_new0(Econtext, 1);
    gint i;

    for (i = SPIN_QUEUE_SIZE; i--;)
    {
        self->queue[i].sound = G_MEMORY_OUTPUT_STREAM(
                g_memory_output_stream_new(NULL, 0, realloc, free));
        self->queue[i].state = IN;
    }

    self->in = self->queue;
    self->process = self->queue;
    self->out = self->queue;

    self->process_chunk = g_slist_alloc();
    self->process_chunk->data = self;
    self->closure = closure;

    pthread_mutex_init(&self->lock, NULL);
    pthread_cond_init(&self->cond, NULL);

    GST_DEBUG("[%p]", self);

    return self;
}

void
spin_unref(Econtext *self)
{
    GST_DEBUG("[%p]", self);

    gint i;

    for (i = SPIN_QUEUE_SIZE; i--;)
    {
        g_output_stream_close(G_OUTPUT_STREAM(self->queue[i].sound),
                NULL, NULL);
        g_object_unref(self->queue[i].sound);
    }

    pthread_cond_destroy(&self->cond);
    pthread_mutex_destroy(&self->lock);

    g_slist_free(self->process_chunk);

    g_free(self);
    memset(self, 0, sizeof(Econtext));
}

// in/out ----------------------------------------------------------------------

void
spin_in(Econtext *self, const gchar *str_)
{
    GST_DEBUG("[%p] str=%s", self, str_);

    if (str_ == NULL || *str_ == 0)
        return;

    Text *str = string_new(str_);

    if (self->in_queue)
    {
        self->in_queue = g_slist_append(self->in_queue, str);
        return;
    }

    gsize orig_frame_len = str->frame_len;

    pthread_mutex_lock(&self->lock);

    while (str->frame_len && self->in->state == IN)
    {
        Espin *spin = self->in;

        GST_DEBUG("[%p] str->offset=%ld str->frame_len=%ld "
                  "spin->text.offset=%ld spin->text.frame_len=%ld", self,
                str->offset, str->frame_len,
                spin->text.offset, spin->text.frame_len);

        string_chunk(str, &spin->text, SPIN_FRAME_SIZE);
        spin->state = PROCESS;

        GST_DEBUG("[%p] str->offset=%ld str->frame_len=%ld "
                  "spin->text.offset=%ld spin->text.frame_len=%ld", self,
                str->offset, str->frame_len,
                spin->text.offset, spin->text.frame_len);

        spinning(self->queue, &self->in);
    }

    if ((orig_frame_len != str->frame_len) && (self->state & INPROCESS) == 0)
    {
        GST_DEBUG("[%p] orig_frame_len=%ld str->len=%ld", self, orig_frame_len,
                str->len);

        self->state |= INPROCESS;
        process_push(self);
    }

    pthread_mutex_unlock(&self->lock);

    if (!string_nil(str))
        self->in_queue = g_slist_append(self->in_queue, str);
}

gpointer
spin_out(Econtext *self, gsize *size_to_play)
{
    GST_DEBUG("[%p] size_to_play=%d", self, *size_to_play);

    gpointer out = NULL;
    pthread_mutex_lock(&self->lock);

    for (;;)
    {
        while ((self->state & CLOSE) == 0 && (self->out->state & (PLAY|OUT))
                == 0)
            pthread_cond_wait(&self->cond, &self->lock);

        GST_DEBUG("[%p] self->state=%d self->out->state=%d", self,
                self->state, self->out->state);

        if (self->state & CLOSE)
            break;

        Espin *spin = self->out;
        gsize spin_size = g_memory_output_stream_get_data_size(spin->sound);

        if (spin->state == PLAY && spin->sound_pos >= spin_size)
        {
            spin->state = IN;
            string_unref(&spin->text);
            spinning(self->queue, &self->out);

            GST_DEBUG("[%p] self->out->state=%d", self, self->out->state);

            continue;
        }

        spin->state = PLAY;
        *size_to_play = MIN(*size_to_play, spin_size);

        out = (guchar*)g_memory_output_stream_get_data(spin->sound) +
                spin->sound_pos;

        spin->sound_pos += *size_to_play;

        GST_DEBUG("[%p] *size_to_play=%ld spin_size=%ld tell=%ld",
                self, *size_to_play, spin_size, spin->sound_pos);

        break;
    }

    pthread_mutex_unlock(&self->lock);

    return out;
}

// process ----------------------------------------------------------------------

static pthread_t process_tid;
static pthread_mutex_t process_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t process_cond = PTHREAD_COND_INITIALIZER;
static GSList *process_queue = NULL;
static EspeakCallBack process_espeak_cb = NULL;

static void*
process(void *data)
{
    pthread_mutex_lock(&process_lock);

    for (;;)
    {
        while (process_queue == NULL)
            pthread_cond_wait(&process_cond, &process_lock);

        while (process_queue)
        {
            Econtext *context = (Econtext*)process_queue->data;
            Espin *spin = context->process;

            process_queue = g_slist_remove_link(process_queue, process_queue);
            gboolean next = FALSE;

            pthread_mutex_unlock(&process_lock);
                gchar *text = spin->text.body + spin->text.offset;
                gchar last_char = text[spin->text.frame_len];
                text[spin->text.frame_len] = 0;

                GST_DEBUG("[%p] text=%s", context, text);

                g_seekable_seek(G_SEEKABLE(spin->sound), 0, G_SEEK_SET,
                        NULL, NULL);
                process_espeak_cb(text, spin->sound, context->closure);
                spin->sound_pos = 0;

                text[spin->text.frame_len] = last_char;

                pthread_mutex_lock(&context->lock);
                    spin->state = OUT;
                    spinning(context->queue, &context->process);
                    next = context->process->state == PROCESS;
                    if (!next)
                        context->state |= INPROCESS;
                pthread_mutex_unlock(&context->lock);
            pthread_mutex_lock(&process_lock);

            if (next)
                process_queue = g_slist_concat(process_queue,
                        context->process_chunk);
        }
    }

    pthread_mutex_unlock(&process_lock);
    return NULL;
}

void
spin_init(EspeakCallBack espeak_cb)
{
    process_espeak_cb = espeak_cb;

    pthread_attr_t attr;
    g_assert(pthread_attr_init(&attr) == 0);
    g_assert(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
    g_assert(pthread_create(&process_tid, &attr, process, NULL) == 0);
    g_assert(pthread_attr_destroy(&attr) == 0);
}

static void
process_push(Econtext *context)
{
    pthread_mutex_lock(&process_lock);
    process_queue = g_slist_concat(process_queue, context->process_chunk);
    pthread_cond_signal(&process_cond);
    pthread_mutex_unlock(&process_lock);
}
