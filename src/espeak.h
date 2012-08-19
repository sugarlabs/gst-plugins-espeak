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

#ifndef ESPEAK_H
#define ESPEAK_H

#define ESPEAK_DEFAULT_VOICE  "default"

#define ESPEAK_TRACK_NONE 0
#define ESPEAK_TRACK_WORD 1
#define ESPEAK_TRACK_MARK 2

struct _Econtext;
typedef struct _Econtext Econtext;

Econtext *espeak_new (GstElement *);
void espeak_unref (Econtext *);

gint espeak_get_sample_rate ();
gint espeak_get_buffer_size ();
GValueArray *espeak_get_voices ();
void espeak_set_pitch (Econtext *, gint);
void espeak_set_rate (Econtext *, gint);
void espeak_set_voice (Econtext *, const gchar *);
void espeak_set_gap (Econtext *, guint);
void espeak_set_track (Econtext *, guint);

void espeak_in (Econtext *, const gchar * str);
GstBuffer *espeak_out (Econtext *, gsize size_to_play);
void espeak_reset (Econtext *);

#endif
