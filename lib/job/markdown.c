/*
 * libjsongit2 - minimal server-side markdown renderer
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
 * This replaces the old client-side showdown + "preprocess the dangerous
 * characters" arrangement.  Raw HTML in the markdown is NEVER passed
 * through: every text byte is HTML-escaped at emission, which removes the
 * XSS class entirely rather than trying to defuse it.  It covers the
 * well-behaved README / blog subset: headings, paragraphs, fenced and
 * indented code blocks, blockquotes, nested lists, pipe tables, rules,
 * inline code / emphasis / strong, links, images and autolinks.
 *
 * Repo-relative image and link URLs are resolved through the caller's
 * callback (images to /plain/, links to /tree/ URLs) like the old
 * sd_ext_plain showdown extension did client-side.
 */

#include "../private.h"

#include <string.h>
#include <stdarg.h>

struct md_ctx {
	struct jg2_hbuf *h;
	const struct jg2_md_ctx *mc;
};

static int md_inline(struct md_ctx *m, const char *s, size_t len);

/* esc: 0 = text node, 1 = attribute value */

static int
md_esc(struct md_ctx *m, const char *s, size_t len, int attr)
{
	size_t n;

	for (n = 0; n < len; n++) {
		const char *rep = NULL;

		switch (s[n]) {
		case '&':
			/*
			 * Escape every ampersand so entities in the source
			 * render literally and nothing downstream can be
			 * tricked into reinterpreting them.
			 */
			rep = "&amp;";
			break;
		case '<':
			rep = "&lt;";
			break;
		case '>':
			rep = "&gt;";
			break;
		case '"':
			if (attr)
				rep = "&quot;";
			break;
		}

		if (rep) {
			if (jg2_hbuf_append(m->h, rep, strlen(rep)))
				return 1;
		} else
			if (jg2_hbuf_append(m->h, s + n, 1))
				return 1;
	}

	return 0;
}

static int
is_url_safe(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-' ||
	       c == '_' || c == '~' || c == '!' || c == '$' || c == '&' ||
	       c == '\'' || c == '(' || c == ')' || c == '*' || c == '+' ||
	       c == ',' || c == ';' || c == '=' || c == ':' || c == '@' ||
	       c == '%';
}

/*
 * Emit a URL attribute: the caller's resolver gets a shot at relative URLs,
 * schemes are restricted to http(s)/#fragment/mailto, and the result is
 * attribute-escaped.
 */

static int
md_url(struct md_ctx *m, int is_image, const char *url, size_t len)
{
	char out[768];
	size_t olen = 0, o = 0;
	const char *emit = url;
	size_t emit_len = len;

	if (!len) {
		emit = "#";
		emit_len = 1;
		goto emit_url;
	}

	if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8) ||
	    !strncmp(url, "mailto:", 7) || url[0] == '#' || url[0] == '/')
		/* pass these through */
		goto emit_url;

	if (!strncmp(url, "javascript:", 11) || !strncmp(url, "data:", 5) ||
	    !strncmp(url, "vbscript:", 9)) {
		emit = "#";
		emit_len = 1;
		goto emit_url;
	}

	/* relative: let the repo resolver rewrite it */

	if (m->mc && m->mc->resolve) {
		olen = m->mc->resolve(m->mc->user, is_image, url, len,
				      out, sizeof(out) - 1);
		if (olen) {
			out[olen] = '\0';
			emit = out;
			emit_len = olen;
		}
	}

emit_url:
	/* belt and braces: percent-encode anything url-unsafe */

	if (emit_len > sizeof(out) - 4)
		emit_len = sizeof(out) - 4;

	o = 0;
	for (size_t i = 0; i < emit_len && o + 3 < sizeof(out); i++) {
		if (is_url_safe(emit[i])) {
			out[o++] = emit[i];
		} else {
			static const char hx[] = "0123456789ABCDEF";
			out[o++] = '%';
			out[o++] = hx[(unsigned char)emit[i] >> 4];
			out[o++] = hx[(unsigned char)emit[i] & 15];
		}
	}
	out[o] = '\0';

	return md_esc(m, out, o, 1);
}

/*
 * Find the end of a markdown link destination that may be <wrapped> or bare,
 * honoring balanced parens.  Returns end pointer or NULL.
 */

static const char *
md_link_dest(const char *s, size_t len, size_t *out_len, const char **title)
{
	const char *p = s, *end = s + len;
	int depth = 0;

	*out_len = 0;
	*title = NULL;

	if (p == end)
		return NULL;

	if (*p == '<') {
		p++;
		const char *e2 = memchr(p, '>', (size_t)(end - p));
		if (!e2)
			return NULL;
		*out_len = (size_t)(e2 - p);

		return e2 + 1 <= end ? e2 + 1 : NULL;
	}

	while (p < end) {
		char c = *p;

		if (c == ' ' || c == '\t' || c == '\n')
			break;
		if (c == '(')
			depth++;
		if (c == ')') {
			if (!depth)
				break;
			depth--;
		}
		p++;
	}

	*out_len = (size_t)(p - s);

	return p;
}

/*
 * Try to parse a link or image at s (which points just after the '[' or
 * '![", with len bytes available).  Returns the number of bytes consumed
 * through the closing ')', or 0 if there is no well-formed construct here.
 */

static size_t
md_autolink_len(const char *s, size_t len)
{
	size_t n;

	if (len < 8)
		return 0;
	if (strncmp(s, "http://", 7) && strncmp(s, "https://", 8))
		return 0;

	for (n = 0; n < len; n++) {
		char c = s[n];

		if (c == ' ' || c == '\t' || c == '\n' || c == '<' ||
		    c == '>' || c == '"' || c == ')' || c == '\'')
			break;
	}

	return n;
}

static size_t
md_try_linkish(struct md_ctx *m, const char *s, size_t len, int is_image)
{
	const char *close = NULL;
	size_t n, text_len;
	const char *u, *uend, *title;
	size_t ulen, remain;

	/* find the first unescaped ']' */

	for (n = 0; n < len; n++) {
		if (s[n] == '\\' && n + 1 < len) {
			n++;
			continue;
		}
		if (s[n] == ']') {
			close = s + n;
			break;
		}
	}

	if (!close)
		return 0;

	text_len = (size_t)(close - s);

	if ((size_t)(close - s) + 2 >= len || close[1] != '(')
		return 0;

	u = close + 2;
	remain = len - (size_t)(u - s);

	uend = md_link_dest(u, remain, &ulen, &title);
	if (!uend || uend == u + remain || *uend != ')')
		return 0;

	if (is_image) {
		if (jg2_hbuf_append(m->h, "<img src=\"", 10) ||
		    md_url(m, 1, u, ulen) ||
		    jg2_hbuf_append(m->h, "\" alt=\"", 7) ||
		    md_esc(m, s, text_len, 1) ||
		    jg2_hbuf_append(m->h, "\">", 2))
			return 0;
	} else {
		if (jg2_hbuf_append(m->h, "<a href=\"", 9) ||
		    md_url(m, 0, u, ulen) ||
		    jg2_hbuf_append(m->h, "\">", 2) ||
		    md_inline(m, s, text_len) ||
		    jg2_hbuf_append(m->h, "</a>", 4))
			return 0;
	}

	return (size_t)(uend - s) + 1; /* through ')' */
}

static int
md_inline(struct md_ctx *m, const char *s, size_t len)
{
	size_t n = 0, k;

	while (n < len) {
		char c = s[n];

		if (c == '\\' && n + 1 < len) {
			/* escaped punctuation renders as itself */

			if (md_esc(m, s + n + 1, 1, 0))
				return 1;
			n += 2;
			continue;
		}

		if (c == '`') {
			const char *f = memchr(s + n + 1, '`', len - n - 1);

			if (f) {
				if (jg2_hbuf_append(m->h, "<code>", 6) ||
				    md_esc(m, s + n + 1,
					   (size_t)(f - (s + n + 1)), 0) ||
				    jg2_hbuf_append(m->h, "</code>", 7))
					return 1;
				n = (size_t)(f - s) + 1;
				continue;
			}
		}

		if (c == '!' && n + 1 < len && s[n + 1] == '[') {
			k = md_try_linkish(m, s + n + 2, len - n - 2, 1);
			if (k) {
				n += k + 2;
				continue;
			}
		}

		if (c == '[') {
			k = md_try_linkish(m, s + n + 1, len - n - 1, 0);
			if (k) {
				n += k + 1;
				continue;
			}
		}

		k = md_autolink_len(s + n, len - n);
		if (k) {
			if (jg2_hbuf_append(m->h, "<a href=\"", 9) ||
			    md_url(m, 0, s + n, k) ||
			    jg2_hbuf_append(m->h, "\">", 2) ||
			    md_esc(m, s + n, k, 0) ||
			    jg2_hbuf_append(m->h, "</a>", 4))
				return 1;
			n += k;
			continue;
		}

		if (c == '*' && n + 1 < len && s[n + 1] == '*') {
			for (k = n + 2; k + 1 < len; k++)
				if (s[k] == '*' && s[k + 1] == '*')
					break;

			if (k + 1 < len && k > n + 2) {
				if (jg2_hbuf_append(m->h, "<strong>", 8) ||
				    md_inline(m, s + n + 2, k - n - 2) ||
				    jg2_hbuf_append(m->h, "</strong>", 9))
					return 1;
				n = k + 2;
				continue;
			}
		}

		if (c == '*' && n + 1 < len) {
			for (k = n + 1; k < len; k++)
				if (s[k] == '*')
					break;

			if (k < len && k > n + 1) {
				if (jg2_hbuf_append(m->h, "<em>", 4) ||
				    md_inline(m, s + n + 1, k - n - 1) ||
				    jg2_hbuf_append(m->h, "</em>", 5))
					return 1;
				n = k + 1;
				continue;
			}
		}

		if (md_esc(m, s + n, 1, 0))
			return 1;
		n++;
	}

	return 0;
}

struct md_line {
	const char *s;
	size_t len;
};

static int
md_lines(struct md_ctx *m, const struct md_line *lines, size_t count);

static int
is_blank(const char *s, size_t len)
{
	size_t n;

	for (n = 0; n < len; n++)
		if (s[n] != ' ' && s[n] != '\t')
			return 0;

	return 1;
}

static int
fence_len(const char *s, size_t len, char fc)
{
	size_t n = 0;

	while (n < len && s[n] == fc)
		n++;

	return n >= 3 ? (int)n : 0;
}

static int
ind(const char *s, size_t len)
{
	size_t n = 0;

	while (n < len && s[n] == ' ')
		n++;

	return (int)n;
}

static int
is_hr(const char *s, size_t len)
{
	size_t n = 0, count = 0;
	char c = 0;

	while (n < len && (s[n] == ' ' || s[n] == '\t'))
		n++;

	if (n == len)
		return 0;

	c = s[n];
	if (c != '-' && c != '*' && c != '_')
		return 0;

	for (; n < len; n++) {
		if (s[n] == c)
			count++;
		else if (s[n] != ' ' && s[n] != '\t')
			return 0;
	}

	return count >= 3;
}

/* table separator line like " | --- | :---: | " */

static int
is_table_sep(const char *s, size_t len)
{
	size_t n;
	int dashes = 0, pipes = 0;

	for (n = 0; n < len; n++) {
		if (s[n] == '-')
			dashes++;
		else if (s[n] == '|')
			pipes++;
		else if (s[n] != ' ' && s[n] != '\t' && s[n] != ':')
			return 0;
	}

	return dashes >= 3 && pipes >= 1;
}

static int
md_emit_table(struct md_ctx *m, const struct md_line *lines, size_t count)
{
	size_t n, i;

	for (n = 0; n < count; n++) {
		const char *s = lines[n].s;
		size_t len = lines[n].len, cs, ce;
		const char *tag = n ? "td" : "th";

		if (jg2_hbuf_append(m->h, "<tr>", 4))
			return 1;

		/* skip a leading pipe */

		cs = 0;
		while (cs < len && s[cs] == ' ')
			cs++;
		if (cs < len && s[cs] == '|')
			cs++;

		/* split the row on unescaped '|' */

		i = cs;
		while (i <= len) {
			if (i > cs && i < len && s[i - 1] == '\\') {
				i++;
				continue;
			}

			if (i == len || s[i] == '|') {
				ce = i;

				/* trim trailing spaces */

				while (ce > cs &&
				       (s[ce - 1] == ' ' || s[ce - 1] == '\t'))
					ce--;

				if (jg2_hbuf_printf(m->h, "<%s>", tag) ||
				    md_inline(m, s + cs, ce - cs) ||
				    jg2_hbuf_printf(m->h, "</%s>", tag))
					return 1;

				cs = i + 1;
			}

			if (i == len)
				break;
			i++;
		}

		if (jg2_hbuf_append(m->h, "</tr>", 5))
			return 1;
	}

	return 0;
}

static int
md_lines(struct md_ctx *m, const struct md_line *lines, size_t count)
{
	size_t n = 0;

	while (n < count) {
		const char *s = lines[n].s;
		size_t len = lines[n].len;
		int fl, i;
		size_t k;

		if (is_blank(s, len)) {
			n++;
			continue;
		}

		/* fenced code */

		fl = fence_len(s, len, '`');
		if (!fl)
			fl = fence_len(s, len, '~');
		if (fl) {
			size_t start = ++n;

			if (jg2_hbuf_append(m->h, "<pre><code>", 11))
				return 1;

			while (n < count &&
			       fence_len(lines[n].s, lines[n].len, s[0]) < fl)
				n++;

			for (k = start; k < n; k++)
				if (md_esc(m, lines[k].s, lines[k].len, 0) ||
				    jg2_hbuf_append(m->h, "\n", 1))
					return 1;

			if (jg2_hbuf_append(m->h, "</code></pre>", 13))
				return 1;

			n++; /* closing fence */
			continue;
		}

		/* heading */

		if (s[0] == '#') {
			int level = 0;

			while (level < (int)len && s[level] == '#' && level < 6)
				level++;

			if (level < (int)len && s[level] == ' ') {
				const char *hs = s + level;
				size_t hl = len - (size_t)level;

				while (hl && hs[hl - 1] == '#')
					hl--;
				while (hl && (hs[hl - 1] == ' '))
					hl--;

				if (jg2_hbuf_printf(m->h, "<h%d>", level) ||
				    md_inline(m, hs, hl) ||
				    jg2_hbuf_printf(m->h, "</h%d>", level))
					return 1;

				n++;
				continue;
			}
		}

		/* horizontal rule */

		if (is_hr(s, len)) {
			if (jg2_hbuf_append(m->h, "<hr>", 4))
				return 1;
			n++;
			continue;
		}

		/* blockquote */

		if (len && s[0] == '>') {
			struct md_line inner[128];
			size_t ic = 0;

			while (n < count && lines[n].len &&
			       lines[n].s[0] == '>') {
				const char *q = lines[n].s + 1;
				size_t ql = lines[n].len - 1;

				if (ql && q[0] == ' ') {
					q++;
					ql--;
				}

				if (ic < LWS_ARRAY_SIZE(inner)) {
					inner[ic].s = q;
					inner[ic].len = ql;
					ic++;
				}
				n++;
			}

			if (jg2_hbuf_append(m->h, "<blockquote>", 12) ||
			    md_lines(m, inner, ic) ||
			    jg2_hbuf_append(m->h, "</blockquote>", 13))
				return 1;

			continue;
		}

		/* table */

		if (n + 1 < count && memchr(s, '|', len) &&
		    is_table_sep(lines[n + 1].s, lines[n + 1].len)) {
			size_t start = n, rows;

			n += 2;

			while (n < count && !is_blank(lines[n].s, lines[n].len)
			       && memchr(lines[n].s, '|', lines[n].len))
				n++;

			rows = n - start;

			if (jg2_hbuf_append(m->h, "<table>", 7) ||
			    md_emit_table(m, lines + start, rows) ||
			    jg2_hbuf_append(m->h, "</table>", 8))
				return 1;

			continue;
		}

		/* list item */

		i = ind(s, len);
		if (i < (int)len &&
		    ((s[i] == '-' || s[i] == '*' || s[i] == '+') &&
		     i + 1 < (int)len && s[i + 1] == ' ')) {

			int ordered = 0;
			(void)ordered;

			if (jg2_hbuf_append(m->h, "<ul>", 4))
				return 1;

			while (n < count) {
				const char *ls;
				size_t ll;
				int li, cur_indent;

				cur_indent = ind(lines[n].s, lines[n].len);
				ls = lines[n].s + cur_indent;
				ll = lines[n].len - (size_t)cur_indent;

				if (is_blank(lines[n].s, lines[n].len))
					break;

				if (!ll || !(ls[0] == '-' || ls[0] == '*' ||
					     ls[0] == '+') || ll < 2 ||
				    ls[1] != ' ')
					break;

				if (jg2_hbuf_append(m->h, "<li>", 4))
					return 1;

				/* the item body: first line remainder plus
				 * any more-indented continuation lines */

				if (md_inline(m, ls + 2, ll - 2))
					return 1;

				n++;

				while (n < count &&
				       !is_blank(lines[n].s, lines[n].len) &&
				       (li = ind(lines[n].s, lines[n].len))) {
					if (md_inline(m,
						lines[n].s + (size_t)li,
						lines[n].len - (size_t)li))
						return 1;
					n++;
				}

				if (jg2_hbuf_append(m->h, "</li>", 5))
					return 1;
			}

			if (jg2_hbuf_append(m->h, "</ul>", 5))
				return 1;

			continue;
		}

		if (i < (int)len && s[i] >= '0' && s[i] <= '9') {
			size_t d = (size_t)i;

			while (d < len && s[d] >= '0' && s[d] <= '9')
				d++;

			if (d < len && s[d] == '.' && d + 1 < len &&
			    s[d + 1] == ' ') {
				if (jg2_hbuf_append(m->h, "<ol>", 4))
					return 1;

				while (n < count) {
					const char *ls;
					size_t ll, dd;
					int cur_indent;

					if (is_blank(lines[n].s, lines[n].len))
						break;

					cur_indent = ind(lines[n].s,
							 lines[n].len);
					ls = lines[n].s + cur_indent;
					ll = lines[n].len - (size_t)cur_indent;

					dd = 0;
					while (dd < ll && ls[dd] >= '0' &&
					       ls[dd] <= '9')
						dd++;

					if (!dd || dd >= ll || ls[dd] != '.' ||
					    dd + 1 >= ll || ls[dd + 1] != ' ')
						break;

					if (jg2_hbuf_append(m->h, "<li>", 4) ||
					    md_inline(m, ls + dd + 2,
						      ll - dd - 2) ||
					    jg2_hbuf_append(m->h, "</li>", 5))
						return 1;

					n++;

					while (n < count &&
					       !is_blank(lines[n].s,
							 lines[n].len) &&
					       ind(lines[n].s,
						   lines[n].len)) {
						int li2 = ind(lines[n].s,
							      lines[n].len);
						if (md_inline(m,
							lines[n].s + (size_t)li2,
							lines[n].len -
							(size_t)li2))
							return 1;
						n++;
					}
				}

				if (jg2_hbuf_append(m->h, "</ol>", 5))
					return 1;

				continue;
			}
		}

		/* indented code block */

		if (i >= 4) {
			if (jg2_hbuf_append(m->h, "<pre><code>", 11))
				return 1;

			while (n < count && (is_blank(lines[n].s,
						       lines[n].len) ||
					     ind(lines[n].s,
						 lines[n].len) >= 4)) {
				int ci = ind(lines[n].s, lines[n].len);
				const char *cs = lines[n].s +
						(ci > 4 ? ci : 4);
				size_t cl = lines[n].len - (ci > 4 ?
						(size_t)ci : 4);

				if (md_esc(m, cs, cl, 0) ||
				    jg2_hbuf_append(m->h, "\n", 1))
					return 1;
				n++;
			}

			if (jg2_hbuf_append(m->h, "</code></pre>", 13))
				return 1;

			continue;
		}

		/* paragraph */

		{
			size_t start = n;

			while (n < count && !is_blank(lines[n].s, lines[n].len)
			       && !fence_len(lines[n].s, lines[n].len, '`')
			       && !fence_len(lines[n].s, lines[n].len, '~')
			       && lines[n].s[0] != '#'
			       && lines[n].s[0] != '>'
			       && ind(lines[n].s, lines[n].len) < 4
			       && !is_hr(lines[n].s, lines[n].len))
				n++;

			if (n == start)
				n++;
			else {
				size_t pl = 0, off = 0;

				if (jg2_hbuf_append(m->h, "<p>", 3))
					return 1;

				for (k = start; k < n; k++) {
					if (pl &&
					    jg2_hbuf_append(m->h, "\n", 1))
						return 1;
					pl = 1;
					off = 0;

					/* strip one leading indent */

					while (off < lines[k].len &&
					       lines[k].s[off] == ' ')
						off++;

					if (md_inline(m, lines[k].s + off,
						      lines[k].len - off))
						return 1;
				}

				if (jg2_hbuf_append(m->h, "</p>", 4))
					return 1;
			}

			continue;
		}
	}

	return 0;
}

int
jg2_markdown(struct jg2_hbuf *h, const char *md, size_t len,
	     const struct jg2_md_ctx *mc)
{
	struct md_line lines[2048];
	struct md_ctx m;
	size_t count = 0, ls = 0, n;

	m.h = h;
	m.mc = mc;

	for (n = 0; n <= len; n++) {
		if (n == len || md[n] == '\n') {
			size_t ll = n - ls;

			/* tolerate CRLF */

			if (ll && md[n - 1] == '\r')
				ll--;

			if (count < LWS_ARRAY_SIZE(lines)) {
				lines[count].s = md + ls;
				lines[count].len = ll;
				count++;
			}

			ls = n + 1;
		}
	}

	return md_lines(&m, lines, count);
}
