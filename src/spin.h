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

#ifndef SPIN_H
#define SPIN_H

struct _Econtext;
typedef struct _Econtext Econtext;

typedef void (*EspeakCallBack)(const gchar*, GMemoryOutputStream*, gpointer);

void                spin_init(EspeakCallBack);
struct _Econtext*   spin_new();
void                spin_unref(struct _Econtext*);
void                spin_in(struct _Econtext*, const gchar *str);
gpointer            spin_out(struct _Econtext*, gsize *size_to_play);

#endif
