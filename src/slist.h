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

#ifndef SLIST_H
#define SLIST_H

typedef struct
{
    GSList *list;
    GMutex *lock;
} SList;

inline void
slist_new(SList *self)
{
    self->list = NULL;
    self->lock = g_mutex_new();
}

inline void
slist_clean(SList *self)
{
    GSList *i;
    g_mutex_lock(self->lock);
    for (i = self->list; i; i = g_slist_next(i))
        text_unref(i->data);
    g_mutex_unlock(self->lock);
}

inline void
slist_free(SList *self)
{
    slist_clean(self);
    g_slist_free(self->list);
    g_mutex_free(self->lock);
}

inline gboolean
slist_empty(SList *self)
{
    g_mutex_lock(self->lock);
    gboolean out = self->list == NULL;
    g_mutex_unlock(self->lock);
    return out;
}

inline GSList*
slist_pop_link(SList *self)
{
    GSList *out = NULL;

    g_mutex_lock(self->lock);
    out = self->list;
    self->list = g_slist_remove_link(self->list, self->list);
    g_mutex_unlock(self->lock);

    return out;
}

inline void
slist_push(SList *self, Text *data)
{
    g_mutex_lock(self->lock);
    self->list = g_slist_append(self->list, data);
    g_mutex_unlock(self->lock);
}

inline void
slist_push_link(SList *self, GSList *link)
{
    g_mutex_lock(self->lock);
    self->list = g_slist_concat(link, self->list);
    g_mutex_unlock(self->lock);
}

#endif
