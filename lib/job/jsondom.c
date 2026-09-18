/*
 * libjsongit2 - minimal JSON DOM for the server-side renderer
 *
 * Copyright (C) 2026 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 *
 * The job emitters produce JSON that the server-side renderer consumes
 * again.  This is a small, strict-enough DOM parser for that internal,
 * well-formed input: it decodes the \uXXXX HTML-safe escapes the emitters
 * use back into raw characters, so the renderer must apply its own HTML
 * escaping at emission time.
 *
 * Nodes and decoded strings are allocated from a single lwsac that the
 * caller frees with lwsac_free() in one go.
 */

#include "../private.h"

#include <string.h>
#include <stdlib.h>

struct jg2_jp {
	struct lwsac **ac;
	const char *p, *end;
	int depth;
};

#define JG2_JN_MAX_DEPTH 24

static struct jg2_jn *
jn_new(struct jg2_jp *jp, char type)
{
	struct jg2_jn *n = lwsac_use(jp->ac, sizeof(*n), 0);

	if (!n)
		return NULL;

	memset(n, 0, sizeof(*n));
	n->type = type;

	return n;
}

static int
jn_ws(struct jg2_jp *jp)
{
	while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' ||
				  *jp->p == '\n' || *jp->p == '\r'))
		jp->p++;

	return jp->p < jp->end;
}

static int
utf8_emit(char **q, char *end, unsigned int cp)
{
	char *o = *q;

	if (cp < 0x80) {
		if (o + 1 > end)
			return 1;
		*o++ = (char)cp;
	} else if (cp < 0x800) {
		if (o + 2 > end)
			return 1;
		*o++ = (char)(0xc0 | (cp >> 6));
		*o++ = (char)(0x80 | (cp & 0x3f));
	} else if (cp < 0x10000) {
		if (o + 3 > end)
			return 1;
		*o++ = (char)(0xe0 | (cp >> 12));
		*o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
		*o++ = (char)(0x80 | (cp & 0x3f));
	} else {
		if (o + 4 > end)
			return 1;
		*o++ = (char)(0xf0 | (cp >> 18));
		*o++ = (char)(0x80 | ((cp >> 12) & 0x3f));
		*o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
		*o++ = (char)(0x80 | (cp & 0x3f));
	}

	*q = o;

	return 0;
}

static int
hex4(const char *p, unsigned int *cp)
{
	unsigned int v = 0;
	int n;

	for (n = 0; n < 4; n++) {
		char c = p[n];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned)(c - 'A' + 10);
		else
			return 1;
	}

	*cp = v;

	return 0;
}

/*
 * Parse a JSON string starting at the opening quote.  The decoded,
 * NUL-terminated result is allocated on the lwsac and set into *result.
 */

static int
jn_string(struct jg2_jp *jp, char **result, size_t *rlen)
{
	const char *start = ++jp->p;
	char *buf, *q, *end;

	/* find the closing quote, so we can bound the decode buffer */

	while (jp->p < jp->end && *jp->p != '"') {
		if (*jp->p == '\\' && jp->p + 1 < jp->end)
			jp->p++;
		jp->p++;
	}

	if (jp->p == jp->end)
		return 1;

	/* worst case every input byte becomes 4 output bytes + NUL */

	buf = lwsac_use(jp->ac, (size_t)(jp->p - start) * 4 + 1, 0);
	if (!buf)
		return 1;

	q = buf;
	end = buf + (size_t)(jp->p - start) * 4;

	jp->p = start;

	while (jp->p < jp->end && *jp->p != '"') {
		char c = *jp->p++;

		if (c != '\\') {
			if (q < end)
				*q++ = c;
			continue;
		}

		if (jp->p == jp->end)
			return 1;

		c = *jp->p++;

		switch (c) {
		case '"':
		case '\\':
		case '/':
			if (q < end)
				*q++ = c;
			break;
		case 'b':
			if (q < end)
				*q++ = '\b';
			break;
		case 'f':
			if (q < end)
				*q++ = '\f';
			break;
		case 'n':
			if (q < end)
				*q++ = '\n';
			break;
		case 'r':
			if (q < end)
				*q++ = '\r';
			break;
		case 't':
			if (q < end)
				*q++ = '\t';
			break;
		case 'u': {
			unsigned int cp;

			if (jp->p + 4 > jp->end || hex4(jp->p, &cp))
				return 1;
			jp->p += 4;

			/* surrogate pair? */

			if (cp >= 0xd800 && cp <= 0xdbff &&
			    jp->p + 6 <= jp->end &&
			    jp->p[0] == '\\' && jp->p[1] == 'u') {
				unsigned int lo;

				if (!hex4(jp->p + 2, &lo) &&
				    lo >= 0xdc00 && lo <= 0xdfff) {
					cp = 0x10000 +
					     ((cp - 0xd800) << 10) +
					     (lo - 0xdc00);
					jp->p += 6;
				}
			}

			if (utf8_emit(&q, end, cp))
				return 1;
			break;
		}
		default:
			return 1;
		}
	}

	jp->p++; /* closing quote */
	*q = '\0';

	*result = buf;
	if (rlen)
		*rlen = (size_t)(q - buf);

	return 0;
}

static struct jg2_jn *
jn_value(struct jg2_jp *jp);

static int
jn_key(struct jg2_jp *jp, char **key)
{
	if (!jn_ws(jp) || *jp->p != '"')
		return 1;

	return jn_string(jp, key, NULL);
}

static struct jg2_jn *
jn_object(struct jg2_jp *jp)
{
	struct jg2_jn *obj = jn_new(jp, JG2_JN_OBJ), **tail = &obj->child;

	if (!obj)
		return NULL;

	jp->p++; /* '{' */

	if (!jn_ws(jp))
		return NULL;

	if (*jp->p == '}') {
		jp->p++;
		return obj;
	}

	while (1) {
		struct jg2_jn *m = jn_new(jp, JG2_JN_OBJ);

		if (!m)
			return NULL;
		if (jn_key(jp, &m->key))
			return NULL;
		if (!jn_ws(jp) || *jp->p != ':')
			return NULL;
		jp->p++;

		m->child = jn_value(jp);
		if (!m->child)
			return NULL;

		*tail = m;
		tail = &m->next;

		if (!jn_ws(jp))
			return NULL;
		if (*jp->p == ',') {
			jp->p++;
			continue;
		}
		if (*jp->p == '}') {
			jp->p++;
			return obj;
		}

		return NULL;
	}
}

static struct jg2_jn *
jn_array(struct jg2_jp *jp)
{
	struct jg2_jn *arr = jn_new(jp, JG2_JN_ARR), **tail = &arr->child;

	if (!arr)
		return NULL;

	jp->p++; /* '[' */

	if (!jn_ws(jp))
		return NULL;

	if (*jp->p == ']') {
		jp->p++;
		return arr;
	}

	while (1) {
		struct jg2_jn *e = jn_value(jp);

		if (!e)
			return NULL;

		*tail = e;
		tail = &e->next;

		if (!jn_ws(jp))
			return NULL;
		if (*jp->p == ',') {
			jp->p++;
			continue;
		}
		if (*jp->p == ']') {
			jp->p++;
			return arr;
		}

		return NULL;
	}
}

static struct jg2_jn *
jn_value(struct jg2_jp *jp)
{
	struct jg2_jn *n;

	if (jp->depth >= JG2_JN_MAX_DEPTH || !jn_ws(jp))
		return NULL;

	jp->depth++;

	switch (*jp->p) {
	case '{':
		n = jn_object(jp);
		break;
	case '[':
		n = jn_array(jp);
		break;
	case '"':
		n = jn_new(jp, JG2_JN_STR);
		if (!n)
			break;
		if (jn_string(jp, &n->str, &n->str_len))
			n = NULL;
		break;
	case 't':
		if (jp->end - jp->p < 4 || strncmp(jp->p, "true", 4)) {
			n = NULL;
			break;
		}
		jp->p += 4;
		n = jn_new(jp, JG2_JN_TRUE);
		break;
	case 'f':
		if (jp->end - jp->p < 5 || strncmp(jp->p, "false", 5)) {
			n = NULL;
			break;
		}
		jp->p += 5;
		n = jn_new(jp, JG2_JN_FALSE);
		break;
	case 'n':
		if (jp->end - jp->p < 4 || strncmp(jp->p, "null", 4)) {
			n = NULL;
			break;
		}
		jp->p += 4;
		n = jn_new(jp, JG2_JN_NULL);
		break;
	default: {
		const char *start = jp->p;

		while (jp->p < jp->end && ((*jp->p >= '0' && *jp->p <= '9') ||
					   *jp->p == '-' || *jp->p == '+' ||
					   *jp->p == '.' || *jp->p == 'e' ||
					   *jp->p == 'E'))
			jp->p++;

		if (jp->p == start) {
			n = NULL;
			break;
		}

		n = jn_new(jp, JG2_JN_NUM);
		if (!n)
			break;

		n->str = lwsac_use(jp->ac, (size_t)(jp->p - start) + 1, 0);
		if (!n->str) {
			n = NULL;
			break;
		}

		memcpy(n->str, start, (size_t)(jp->p - start));
		n->str[(size_t)(jp->p - start)] = '\0';
		n->str_len = (size_t)(jp->p - start);
		break;
	}
	}

	jp->depth--;

	return n;
}

/*
 * Parse a sequence of top-level JSON values (the internal emitter separates
 * chained job items with a comma, but we also tolerate them jammed together
 * with nothing between).  Returns a JG2_JN_ARR whose elements are the parsed
 * values, or NULL on malformed input.
 */

struct jg2_jn *
jg2_json_parse_seq(const char *buf, size_t len, struct lwsac **ac)
{
	struct jg2_jp jp;
	struct jg2_jn *seq, **tail;

	*ac = NULL;

	jp.ac = ac;
	jp.p = buf;
	jp.end = buf + len;
	jp.depth = 0;

	seq = jn_new(&jp, JG2_JN_ARR);
	if (!seq)
		return NULL;

	tail = &seq->child;

	while (jn_ws(&jp)) {

		/* the emitters separate items with ','; skip any number */

		while (jp.p < jp.end && *jp.p == ',')
			jp.p++;

		if (!jn_ws(&jp))
			break;

		*tail = jn_value(&jp);
		if (!*tail)
			goto bail;
		tail = &(*tail)->next;
	}

	return seq;

bail:
	lwsac_free(ac);

	return NULL;
}

const struct jg2_jn *
jg2_jn_obj_get(const struct jg2_jn *obj, const char *key)
{
	const struct jg2_jn *n;

	if (!obj || obj->type != JG2_JN_OBJ)
		return NULL;

	n = obj->child;
	while (n) {
		if (n->key && !strcmp(n->key, key))
			return n->child;
		n = n->next;
	}

	return NULL;
}

long long
jg2_jn_ll(const struct jg2_jn *n)
{
	if (!n || (n->type != JG2_JN_NUM && n->type != JG2_JN_STR) || !n->str)
		return 0;

	return atoll(n->str);
}

const char *
jg2_jn_str(const struct jg2_jn *n)
{
	if (!n || n->type != JG2_JN_STR)
		return NULL;

	return n->str;
}

/* like jg2_jn_str(), but also returns the decoded length */

const char *
jg2_jn_str2(const struct jg2_jn *n, size_t *len)
{
	if (!n || n->type != JG2_JN_STR)
		return NULL;

	if (len)
		*len = n->str_len;

	return n->str;
}
