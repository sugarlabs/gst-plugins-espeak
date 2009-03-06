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

#ifndef TEXT_H
#define TEXT_H

#include <string.h>

typedef struct
{
    goffset offset;
    gsize frame_len;
    gsize len;
    gchar *body;
} Text;

inline Text*
text_new(const gchar *src)
{
    if (!src)
        return NULL;

    gsize len = strlen(src);

    Text *out = (Text*)g_malloc(sizeof(Text) + len + 1);
    gchar *body = (gchar*)out + sizeof(Text);

    out->offset = 0;
    out->frame_len = len;
    out->len = len;
    out->body = body;

    memcpy(body, src, len + 1);

    GST_DEBUG("[%p] len=%ld", out, len);

    return out;
}

inline void
text_chunk(Text *src, Text *dst, gsize len)
{
    memcpy(dst, src, sizeof(Text));

    gsize dst_len = MIN(len, src->frame_len);
    gchar *dst_last = dst->body + dst->offset + dst_len;
    gchar *i;

    if (dst_len < src->frame_len)
        for (i = dst_last; dst_len; --dst_len, --i)
            if (g_ascii_isspace(*i))
                break;

    if (dst_len)
        dst->frame_len = dst_len;
    else
    {
        dst_last = g_utf8_prev_char(dst_last + 1);
        dst->frame_len = dst_last - (dst->body + dst->offset);
    }

    src->offset += dst->frame_len;
    src->frame_len -= dst->frame_len;

    GST_DEBUG("[%p] len=%ld dst_len=%ld dst_last=%ld "
              "src->offset=%ld src->frame_len=%ld", src, len, dst_len,
            dst_last-dst->body, src->offset, src->frame_len);
}

inline gchar*
text_first(Text *self)
{
    return self->body + self->offset;
}

inline gchar*
text_last(Text *self)
{
    return text_first(self) + self->frame_len;
}

inline gboolean
text_eot(Text *str)
{
    return str->frame_len == 0;
}

inline void
text_unref(Text *str)
{
    if (text_eot(str))
        return;

    gpointer data = NULL;

    if (str->offset + str->frame_len >= str->len)
        data = str->body - sizeof(Text);

    memset(str, 0, sizeof(Text));

    GST_DEBUG("[%p]", data);

    g_free(data);
}

#endif
