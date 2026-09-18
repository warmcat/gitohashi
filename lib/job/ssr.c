/*
 * libjsongit2 - server-side rendering of job JSON into HTML
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
 * This replaces the client-side jg2.js renderer.  The job emitters still
 * produce the same JSON, but their output is captured into a per-context
 * buffer, parsed and rendered into HTML here, and that HTML is what is
 * spooled to the client and stored in the ref-hash keyed cache.
 *
 * Everything in the JSON is untrusted (repo content): all text emissions
 * go through ssr_esc(), attributes through ssr_esc_attr(), and URL path
 * pieces through ssr_urlenc().  The HTML structure and CSS class names
 * intentionally mirror what jg2.js produced, so the existing jg2.css
 * styles the server-rendered pages unchanged.
 */

#include "../private.h"

#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

/*
 * lws-hl.h arrives via the libwebsockets.h umbrella on lws versions that
 * have the highlighter; LWS_WITH_HL from lws_config.h tells us it is there.
 */

/*
 * Server-side syntax highlighting (lws streaming tokenizer, hostile-input
 * hardened, alloc-free).  Applied to C sources in file and blame views and
 * to commit diffs; the markup is part of the cached HTML.  Naked modes
 * (plain/patch) stay unhighlighted by design.  Without LWS_WITH_HL the
 * emission falls back to plain escaping.
 */

#if defined(LWS_WITH_HL)

struct hl_sink {
	struct jg2_hbuf *h;
	char fail;
};

static lws_stateful_ret_t
hl_write_cb(void *user, const uint8_t *buf, size_t len)
{
	struct hl_sink *s = (struct hl_sink *)user;

	if (jg2_hbuf_append(s->h, (const char *)buf, len)) {
		s->fail = 1;

		return LWS_SRET_FATAL;
	}

	return LWS_SRET_OK;
}

/*
 * Feed src[0..len) through a fresh tokenizer of the given language and emit
 * the stock html markup sink output into h.  The sink escapes everything, so
 * this is safe on arbitrary bytes.  Returns 0 for OK.
 */

static int
hl_emit(struct jg2_hbuf *h, const lws_hl_ops_t *lang, const char *src,
	size_t len)
{
	lws_hl_ctx_t ctx;
	lws_hl_html_t html;
	struct hl_sink s;
	const uint8_t *p = (const uint8_t *)src;
	size_t l = len;

	s.h = h;
	s.fail = 0;

	if (lws_hl_html_construct(&html, hl_write_cb, &s, NULL) ||
	    lws_hl_construct(&ctx, lang, lws_hl_html_token, &html))
		return 1;

	while (l && !s.fail) {
		size_t was = l;

		if (lws_hl_parse(&ctx, &p, &l))
			break;	/* sink failure (never defers) */
		if (l == was)
			break;	/* held decision byte, finish resolves */
	}

	if (!s.fail && lws_hl_finish(&ctx))
		s.fail = 1;
	if (!s.fail && lws_hl_html_close(&html))
		s.fail = 1;

	return s.fail;
}

/* language pick for a filename: NULL = no highlighting */

static const lws_hl_ops_t *
hl_lang_for(const char *name)
{
	size_t n;

	if (!name)
		return NULL;

	n = strlen(name);

#define hl_ends_with(_s) (n > sizeof(_s) - 1 && \
			  !strcmp(name + n - (sizeof(_s) - 1), _s))

#if defined(LWS_WITH_HL_LANG_C)
	if (hl_ends_with(".c") || hl_ends_with(".h") ||
	    hl_ends_with(".cc") || hl_ends_with(".cpp") ||
	    hl_ends_with(".hpp"))
		return lws_hl_lang_c();
#endif

#undef hl_ends_with

	return NULL;
}

/*
 * Highlight a whole source buffer chosen by filename.  Returns 0 if markup
 * was emitted, nonzero if the caller should fall back to plain escaping.
 */

static int
hl_emit_file(struct jg2_hbuf *h, const char *name, const char *src, size_t len)
{
	const lws_hl_ops_t *lang = hl_lang_for(name);

	if (!lang)
		return 1;

	return hl_emit(h, lang, src, len);
}

#if defined(LWS_WITH_HL_LANG_DIFF)
#define JG2_HAVE_HL_DIFF 1
#endif

#else /* ! LWS_WITH_HL */

static int
hl_emit_file(struct jg2_hbuf *h, const char *name, const char *src, size_t len)
{
	(void)h;
	(void)name;
	(void)src;
	(void)len;

	return 1;
}

#endif

/*
 * The internal capture is bounded by this; beyond it we stop capturing and
 * render a "too large" notice instead.  Pages in the multi-megabyte class
 * are pathological for a web view anyway and /plain/ exists for fetching.
 */

#define JG2_SSR_MAX_CAPTURE (32 * 1024 * 1024)
#define JG2_SSR_CAPTURE_SLICE 8192
#define JG2_SSR_BITBUCKET 4096

/* ----------------------------------------------------------------------
 * growing HTML buffer helpers
 */

int
jg2_hbuf_append(struct jg2_hbuf *h, const char *s, size_t len)
{
	if (h->len + len + 1 > h->size) {
		size_t ns = h->size ? h->size : 4096;
		char *nb;

		while (ns < h->len + len + 1)
			ns *= 2;

		nb = realloc(h->buf, ns);
		if (!nb)
			return 1;

		h->buf = nb;
		h->size = ns;
	}

	memcpy(h->buf + h->len, s, len);
	h->len += len;
	h->buf[h->len] = '\0';

	return 0;
}

int
jg2_hbuf_printf(struct jg2_hbuf *h, const char *fmt, ...)
{
	va_list ap;
	char stackbuf[1024];
	int n;
	size_t need;

	va_start(ap, fmt);
	n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
	va_end(ap);

	if (n < 0)
		return 1;

	need = (size_t)n;

	if (need < sizeof(stackbuf))
		return jg2_hbuf_append(h, stackbuf, need);

	/* long result: format again into an exact-size temp */

	{
		char *tmp = malloc(need + 1);
		int ret;

		if (!tmp)
			return 1;

		va_start(ap, fmt);
		vsnprintf(tmp, need + 1, fmt, ap);
		va_end(ap);

		ret = jg2_hbuf_append(h, tmp, need);
		free(tmp);

		return ret;
	}
}

/* ----------------------------------------------------------------------
 * locale / i18n
 */

unsigned char
jg2_ssr_locale_from_alang(const char *alang)
{
	/*
	 * Mirrors the client-side selection in jg2.js: the first matching
	 * language tag in the Accept-Language list wins.
	 */

	const char *p = alang;

	if (!p)
		return 0;

	while (*p) {
		const char *e = p, *semi = NULL;
		size_t l;

		while (*e && *e != ',') {
			if (*e == ';' && !semi)
				semi = e;
			e++;
		}

		l = (size_t)((semi ? semi : e) - p);

		if (l >= 2 && l <= 7) {
			if (l == 2 && !strncmp(p, "ja", 2))
				return 1;
			if (l >= 2 && !strncmp(p, "zh", 2)) {
				if (l == 2)
					return 3;
				if (p[2] == '-') {
					const char *v = p + 3;
					size_t vl = l - 3;

					if ((vl == 2 &&
					     (!strncmp(v, "TW", 2) ||
					      !strncmp(v, "HK", 2) ||
					      !strncmp(v, "SG", 2))) ||
					    (vl == 4 &&
					     !strncmp(v, "HANT", 4)))
						return 2;
					if ((vl == 2 &&
					     !strncmp(v, "CN", 2)) ||
					    (vl == 4 &&
					     !strncmp(v, "HANS", 4)))
						return 3;
				}
			}
			if (l == 2 && !strncmp(p, "en", 2))
				return 0;
		}

		p = *e ? e + 1 : e;
	}

	return 0;
}

struct jg2_i18n_entry {
	const char *en, *ja, *zht, *zhs;
};

static const struct jg2_i18n_entry jg2_i18n_table[] = {
	{ "Summary",	"概要",		"概要",		"概要" },
	{ "Log",	"ログ",		"日誌",		"日志" },
	{ "Tree",	"木構造",	"樹",		"木" },
	{ "Blame",	"責任",		"責怪",		"归咎" },
	{ "Mode",	"モード",	"模式",		"模式" },
	{ "Size",	"サイズ",	"尺寸",		"尺寸" },
	{ "Name",	"名",		"名稱",		"名称" },
	{ "s",		"秒",		"秒",		"秒" },
	{ "m",		"分",		"分鐘",		"分钟" },
	{ "h",		"時間",		"小時",		"小時" },
	{ " days",	"日々",		"天",		"天" },
	{ " weeks",	"週",		"週",		"周" },
	{ " months",	"数ヶ月",	"個月",		"个月" },
	{ " years",	"年",		"年份",		"年份" },
	{ "Branch Snapshot",	"ブランチスナップショット",
					"科快照",	"科快照" },
	{ "Tag Snapshot",	"タグスナップショット",
					"标签快照",	"标签快照" },
	{ "Commit Snapshot",	"スナップショットをコミットする",
					"提交快照",	"提交快照" },
	{ "Description",	"説明",	"描述",		"描述" },
	{ "Owner",	"オーナー",	"所有者",	"所有者" },
	{ "Branch",	"ブランチ",	"科",		"科" },
	{ "Tag",	"タグ",		"標籤",		"标签" },
	{ "Author",	"著者",		"作者",		"作者" },
	{ "Age",	"年齢",		"年齡",		"年龄" },
	{ "Message",	"メッセージ",	"信息",		"信息" },
	{ "Download",	"ダウンロード",	"下載",		"下载" },
	{ "root",	"ルート",	"根源",		"根源" },
	{ "Committer",	"コミッター",	"提交者",	"提交者" },
	{ "Raw Patch",	"生パッチ",	"原始補丁",	"原始补丁" },
	{ "Page fetched %{pf} ago, creation time: %{ct}ms "
	  "(vhost etag hits: %{ve}%, cache hits: %{ch}%)",
	  "%{pf}間前に取得されたページ, 作成にかかった時間: %{ct}ms "
	  "(vhost etag キャッシュヒット: %{ve}%, キャッシュヒット: %{ch}%)",
	  "頁面%{pf}前獲取, 創作時間: %{ct}ms "
	  "(vhost etag 緩存命中: %{ve}%, 緩存命中: %{ch}%)",
	  "页面%{pf}前获取, 创作时间: %{ct}ms "
	  "(vhost etag 缓存命中: %{ve}%, 缓存命中: %{ch}%)" },
};

/* ----------------------------------------------------------------------
 * renderer state
 */

struct jg2_srr {
	struct jg2_ctx *ctx;
	struct jg2_hbuf *h;

	const char *vpath;
	const char *reponame;
	const char *mode;	/* canonicalized display mode */
	const char *rpath;
	const char *qbranch, *qid, *qofs, *qsearch;

	const char *avatar_base; /* proxied (ends '/') or gravatar */
	int avatar_proxied;

	int caps;		/* like the js j.f: 1 blame, 2 archive,
				 * 4 blog mode, 8 blame overloaded */
	unsigned char locale;

	struct {
		const char *desc, *url;
		const struct jg2_jn *owner;
	} repo;

	time_t now;

	/* directory of the blob being markdowned, for relative links */
	char doc_dir[256];
};

static const char *
i18n(struct jg2_srr *r, const char *en)
{
	size_t n;

	if (!r->locale)
		return en;

	for (n = 0; n < LWS_ARRAY_SIZE(jg2_i18n_table); n++) {
		const struct jg2_i18n_entry *e = &jg2_i18n_table[n];

		if (strcmp(e->en, en))
			continue;

		switch (r->locale) {
		case 1:
			return e->ja;
		case 2:
			return e->zht;
		case 3:
			return e->zhs;
		}

		break;
	}

	return en;
}

#define HAP(r, lit) jg2_hbuf_append((r)->h, (lit), sizeof(lit) - 1)

/* ----------------------------------------------------------------------
 * escaping and URL helpers
 */

static int
ssr_esc_(struct jg2_srr *r, const char *s, size_t len, int attr)
{
	size_t n;

	for (n = 0; n < len; n++) {
		const char *rep = NULL;

		switch (s[n]) {
		case '&':
			rep = "&amp;";
			break;
		case '<':
			rep = "&lt;";
			break;
		case '>':
			rep = "&gt;";
			break;
		case '\'':
			rep = "&#39;";
			break;
		case '"':
			if (attr)
				rep = "&quot;";
			break;
		}

		if (rep) {
			if (jg2_hbuf_append(r->h, rep, strlen(rep)))
				return 1;
		} else
			if (jg2_hbuf_append(r->h, s + n, 1))
				return 1;
	}

	return 0;
}

static int
ssr_esc(struct jg2_srr *r, const char *s)
{
	return ssr_esc_(r, s ? s : "", s ? strlen(s) : 0, 0);
}

static int
ssr_esc_attr(struct jg2_srr *r, const char *s)
{
	return ssr_esc_(r, s ? s : "", s ? strlen(s) : 0, 1);
}

static int
ssr_esc_len(struct jg2_srr *r, const char *s, size_t len)
{
	return ssr_esc_(r, s, len, 0);
}

static int
url_unreserved(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
	       c == '~';
}

static int
ssr_urlenc_len(struct jg2_srr *r, const char *s, size_t len, int path)
{
	static const char hx[] = "0123456789ABCDEF";
	size_t n;

	for (n = 0; n < len; n++) {
		char c = s[n];

		if (url_unreserved(c) || (path && c == '/')) {
			if (jg2_hbuf_append(r->h, &c, 1))
				return 1;
		} else {
			char p[3] = { '%', hx[(unsigned char)c >> 4],
				      hx[(unsigned char)c & 15] };

			if (jg2_hbuf_append(r->h, p, 3))
				return 1;
		}
	}

	return 0;
}

#define UQ_BRANCH	1
#define UQ_ID		2
#define UQ_OFS		4
#define UQ_SEARCH	8

/*
 * Appends a link URL like the client-side makeurl().  hovr / idovr, when
 * non-NULL, override (or stand in for) the current h= / id= urlargs.
 */

static int
url_make(struct jg2_srr *r, const char *repo, const char *mode,
	 const char *path, const char *hovr, const char *idovr, int qflags)
{
	const char *h = (qflags & UQ_BRANCH) ? r->qbranch : NULL;
	const char *id = (qflags & UQ_ID) ? r->qid : NULL;
	const char *base = r->vpath;
	size_t bl = strlen(base);
	int qp = 0;

	if (hovr) {
		h = hovr;
		qflags |= UQ_BRANCH;
	}
	if (idovr) {
		id = idovr;
		qflags |= UQ_ID;
	}

	if (repo && repo[0] && bl && base[0] != '/') {
		if (jg2_hbuf_append(r->h, "/", 1))
			return 1;
	}
	if (jg2_hbuf_append(r->h, base, bl))
		return 1;

	if (repo && repo[0]) {
		if (bl && base[bl - 1] != '/' &&
		    jg2_hbuf_append(r->h, "/", 1))
			return 1;
		if (ssr_urlenc_len(r, repo, strlen(repo), 1))
			return 1;
	}

	if (mode) {
		if (jg2_hbuf_append(r->h, "/", 1) ||
		    ssr_urlenc_len(r, mode, strlen(mode), 1))
			return 1;

		if (path && path[0]) {
			if (jg2_hbuf_append(r->h, "/", 1) ||
			    ssr_urlenc_len(r, path, strlen(path), 1))
				return 1;
		}
	}

	if (h) {
		if (jg2_hbuf_append(r->h, qp ? "&h=" : "?h=", 3) ||
		    ssr_urlenc_len(r, h, strlen(h), 0))
			return 1;
		qp = 1;
	}

	if (id) {
		if (jg2_hbuf_append(r->h, qp ? "&id=" : "?id=", 4) ||
		    ssr_urlenc_len(r, id, strlen(id), 0))
			return 1;
		qp = 1;
	}

	if ((qflags & UQ_OFS) && r->qofs) {
		if (jg2_hbuf_append(r->h, qp ? "&ofs=" : "?ofs=", 5) ||
		    ssr_urlenc_len(r, r->qofs, strlen(r->qofs), 0))
			return 1;
		qp = 1;
	}

	if ((qflags & UQ_SEARCH) && r->qsearch) {
		if (jg2_hbuf_append(r->h, qp ? "&q=" : "?q=", 3) ||
		    ssr_urlenc_len(r, r->qsearch, strlen(r->qsearch), 0))
			return 1;
	}

	return 0;
}

/* ----------------------------------------------------------------------
 * widget emitters
 */

static int
emit_identity_img_src(struct jg2_srr *r, const char *md5)
{
	if (!md5)
		return 1;

	if (jg2_hbuf_append(r->h, r->avatar_base, strlen(r->avatar_base)) ||
	    ssr_urlenc_len(r, md5, strlen(md5), 1))
		return 1;

	if (r->avatar_proxied)
		return jg2_hbuf_append(r->h, "_avatar", 7);

	return jg2_hbuf_append(r->h, "?s=128&amp;d=retro", 18);
}

/*
 * Identity widget like the js identity(): sig is the JSON signature object
 * { git_time, name, email, md5 }.  parts bits: 1 = name, 2 = <email>,
 * 4 = date.
 */

static int
emit_identity(struct jg2_srr *r, const struct jg2_jn *sig, int size,
	      int parts)
{
	const char *name, *email, *md5;
	const struct jg2_jn *t;

	if (!sig)
		return 0;

	name = jg2_jn_str(jg2_jn_obj_get(sig, "name"));
	email = jg2_jn_str(jg2_jn_obj_get(sig, "email"));
	md5 = jg2_jn_str(jg2_jn_obj_get(sig, "md5"));

	if (!email || !md5)
		return 0;

	if (HAP(r, "<span class='identity'>") ||
	    HAP(r, "<a class='grav-mailto' href='mailto:") ||
	    ssr_esc_attr(r, email) ||
	    HAP(r, "'>") ||
	    jg2_hbuf_printf(r->h, "<span class='gravatar%d' alt='", size) ||
	    ssr_esc_attr(r, email) ||
	    HAP(r, "'>") ||
	    HAP(r, "<img class=\"inline-identity\" src=\"") ||
	    emit_identity_img_src(r, md5) ||
	    HAP(r, "\" alt=\"[]\">"))
		return 1;

	if ((parts & 1) && name && ssr_esc(r, name))
		return 1;

	if (HAP(r, "</span></a>"))
		return 1;

	if ((parts & 2) && email) {
		if (HAP(r, " &lt;") || ssr_esc(r, email) || HAP(r, "&gt;"))
			return 1;
	}

	if ((parts & 4) && (t = jg2_jn_obj_get(sig, "git_time"))) {
		char dt[96];
		time_t ut = (time_t)jg2_jn_ll(jg2_jn_obj_get(t, "time"));
		struct tm tm;

		if (gmtime_r(&ut, &tm)) {
			strftime(dt, sizeof(dt), "%Y-%m-%d %H:%M UTC", &tm);
			if (jg2_hbuf_printf(r->h, " <span>%s</span>", dt))
				return 1;
		}
	}

	return HAP(r, "</span>");
}

static const struct {
	const char *unit_en;
	long long div, limit;
} jg2_ages[] = {
	{ "s",		1,		120 },
	{ "m",		60,		7200 },
	{ "h",		3600,		172800 },
	{ " days",	86400,		1209600 },
	{ " weeks",	604800,		4838400 },
	{ " months",	2419200,	63072000 },
	{ " years",	31536000,	0 },
};

static int
emit_age_span(struct jg2_srr *r, long long ut)
{
	long long d;
	char dt[96];
	struct tm tm;
	size_t n;

	if (!ut)
		return 0;

	d = (long long)r->now - ut;

	for (n = 0; n < LWS_ARRAY_SIZE(jg2_ages); n++) {
		if (jg2_ages[n].limit && d >= jg2_ages[n].limit)
			continue;

		if (gmtime_r((time_t *)&ut, &tm))
			strftime(dt, sizeof(dt), "%Y-%m-%d %H:%M UTC", &tm);
		else
			dt[0] = '\0';

		return jg2_hbuf_printf(r->h,
			"<span class='age-%d' ut='%lld' title='%s'>%lld%s</span>",
			(int)n, ut, dt,
			(d + jg2_ages[n].div - 1) / jg2_ages[n].div,
			i18n(r, jg2_ages[n].unit_en));
	}

	return 0;
}

/* plain age text (no span), used for the stats line */

static int
emit_age_plain(struct jg2_srr *r, long long ut)
{
	long long d;
	size_t n;

	if (!ut)
		return 0;

	d = (long long)r->now - ut;

	for (n = 0; n < LWS_ARRAY_SIZE(jg2_ages); n++) {
		if (jg2_ages[n].limit && d >= jg2_ages[n].limit)
			continue;

		return jg2_hbuf_printf(r->h, "%lld%s",
			(d + jg2_ages[n].div - 1) / jg2_ages[n].div,
			i18n(r, jg2_ages[n].unit_en));
	}

	return 0;
}

/* branch / tag chips like the js aliases() */

static int
emit_aliases(struct jg2_srr *r, const struct jg2_jn *oidobj)
{
	const struct jg2_jn *alias, *n;

	if (!oidobj)
		return 0;

	alias = jg2_jn_obj_get(oidobj, "alias");
	if (!alias || alias->type != JG2_JN_ARR)
		return 0;

	n = alias->child;
	while (n) {
		const char *ref = jg2_jn_str(n);

		if (ref) {
			if (!strncmp(ref, "refs/heads/", 11)) {
				if (HAP(r, " <span class='inline-branch'>") ||
				    HAP(r, "<span class='alias'>") ||
				    ssr_esc(r, ref + 11) ||
				    HAP(r, "</span></span>"))
					return 1;
			} else if (!strncmp(ref, "refs/tags/", 10)) {
				if (HAP(r, " <span class='inline_tag'>") ||
				    HAP(r, "<div class='alias'>") ||
				    ssr_esc(r, ref + 10) ||
				    HAP(r, "</div></span>"))
					return 1;
			}
		}

		n = n->next;
	}

	return 0;
}

/* ----------------------------------------------------------------------
 * markdown glue: the lws streaming markdown renderer (lws-md,
 * hostile-input hardened) with repo-relative image / link url rewriting.
 * The renderer contexts are some tens of KB, so they are heap-allocated
 * rather than living on the service thread stack.
 */

#if defined(LWS_WITH_MD)

static lws_stateful_ret_t
md_write_cb(void *user, const uint8_t *buf, size_t len)
{
	if (jg2_hbuf_append((struct jg2_hbuf *)user, (const char *)buf, len))
		return LWS_SRET_FATAL;

	return LWS_SRET_OK;
}

static size_t
md_resolve_url(void *user, int is_image, const char *url, size_t len,
	       char *out, size_t out_len)
{
	struct jg2_srr *r = (struct jg2_srr *)user;
	const char *pieces[3];
	size_t plen[3], n = 0, pi, i;
	static const char hx[] = "0123456789ABCDEF";
	const char *m = is_image ? "/plain/" : "/tree/";
	size_t ml = strlen(m), bl = strlen(r->vpath);

	pieces[0] = r->vpath;
	plen[0] = bl;
	pieces[1] = r->reponame ? r->reponame : "";
	plen[1] = strlen(pieces[1]);
	pieces[2] = r->doc_dir;
	plen[2] = strlen(pieces[2]);

	for (pi = 0; pi < 3; pi++) {
		size_t i2;

		for (i2 = 0; i2 < plen[pi]; i2++) {
			char c = pieces[pi][i2];

			if (n + 1 >= out_len)
				return 0;
			out[n++] = c;
		}

		/* separators between the components */

		if (n && out[n - 1] != '/' && n + 1 < out_len)
			out[n++] = '/';
	}

	for (i = 0; i < ml; i++) {
		if (n + 1 >= out_len)
			return 0;
		out[n++] = m[i];
	}

	for (i = 0; i < len; i++) {
		char c = url[i];

		if (url_unreserved(c) || c == '/') {
			if (n + 1 >= out_len)
				return 0;
			out[n++] = c;
		} else {
			if (n + 3 >= out_len)
				return 0;
			out[n++] = '%';
			out[n++] = hx[(unsigned char)c >> 4];
			out[n++] = hx[(unsigned char)c & 15];
		}
	}

	out[n] = '\0';

	return n;
}

static int
emit_markdown(struct jg2_srr *r, const char *md, size_t len)
{
	lws_md_ctx_t *mdctx = calloc(1, sizeof(*mdctx));
	lws_md_html_t *html = calloc(1, sizeof(*html));
	const uint8_t *p = (const uint8_t *)md;
	size_t l = len;
	int ret = 1;

	if (!mdctx || !html)
		goto bail;

	if (lws_md_html_construct(html, md_write_cb, r->h,
				  md_resolve_url, r) ||
	    lws_md_construct(mdctx, lws_md_html_event, html))
		goto bail;

	while (l) {
		size_t was = l;
		lws_stateful_ret_t sr = lws_md_parse(mdctx, &p, &l);

		if (sr)
			goto bail;
		if (l == was) {
			/*
			 * Only legal for a final held CR; finish decides
			 * it.  Anything else means we failed to make
			 * progress.
			 */
			if (l > 1)
				goto bail;
			break;
		}
	}

	if (lws_md_finish(mdctx) || lws_md_html_close(html))
		goto bail;

	ret = 0;

bail:
	free(html);
	free(mdctx);

	return ret;
}

#else /* ! LWS_WITH_MD */

/*
 * Without the renderer in the lws build, degrade to fully-escaped text:
 * nothing from the markdown can become markup, there is just no structure.
 */

static int
emit_markdown(struct jg2_srr *r, const char *md, size_t len)
{
	size_t n;

	if (jg2_hbuf_append(r->h, "<pre>", 5))
		return 1;

	for (n = 0; n < len; n++) {
		const char *rep = NULL;

		switch (md[n]) {
		case '&':	rep = "&amp;";	break;
		case '<':	rep = "&lt;";	break;
		case '>':	rep = "&gt;";	break;
		}

		if (rep ? jg2_hbuf_append(r->h, rep, strlen(rep)) :
			  jg2_hbuf_append(r->h, md + n, 1))
			return 1;
	}

	return jg2_hbuf_append(r->h, "</pre>", 6);
}

#endif /* LWS_WITH_MD */

/* ----------------------------------------------------------------------
 * item renderers
 */

static int
render_repolist(struct jg2_srr *r, const struct jg2_jn *item)
{
	const struct jg2_jn *list, *n;

	list = jg2_jn_obj_get(item, "repolist");
	if (!list || list->type != JG2_JN_ARR)
		return 0;

	if (HAP(r, "<div class='jg2-repolist'><table><tr>"
		  "<td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Name"), strlen(i18n(r, "Name"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Description"),
			    strlen(i18n(r, "Description"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Owner"),
			    strlen(i18n(r, "Owner"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    HAP(r, "URL</td></tr>"))
		return 1;

	n = list->child;
	while (n) {
		const char *rn = jg2_jn_str(jg2_jn_obj_get(n, "reponame"));
		const char *desc = jg2_jn_str(jg2_jn_obj_get(n, "desc"));
		const char *url = jg2_jn_str(jg2_jn_obj_get(n, "url"));

		if (HAP(r, "<tr><td class='dl-dir'><img class='repo oneem'>"
			  "&nbsp;<a class='noline' href='") ||
		    url_make(r, rn, NULL, NULL, NULL, NULL, 0) ||
		    HAP(r, "'>") ||
		    ssr_esc(r, rn) ||
		    HAP(r, "</a></td><td>") ||
		    ssr_esc(r, desc) ||
		    HAP(r, "</td><td>") ||
		    emit_identity(r, n, 16, 1) ||
		    HAP(r, "</td><td>") ||
		    ssr_esc(r, url) ||
		    HAP(r, "</td><td>&nbsp</td></tr>"))
			return 1;

		n = n->next;
	}

	return HAP(r, "</table></div>");
}

/* split a reflist into branch / tag arrays in one pass */

struct jg2_refsplit {
	const struct jg2_jn *branches[512];
	size_t nbranch;
	const struct jg2_jn *tags[512];
	size_t ntag;
	const struct jg2_jn *posts[512];
	size_t npost;
};

static void
reflist_split(const struct jg2_jn *item, struct jg2_refsplit *rs)
{
	const struct jg2_jn *list, *n;

	memset(rs, 0, sizeof(*rs));

	list = jg2_jn_obj_get(item, "reflist");
	if (!list || list->type != JG2_JN_ARR)
		return;

	n = list->child;
	while (n) {
		const char *name = jg2_jn_str(jg2_jn_obj_get(n, "name"));

		if (name) {
			if (!strncmp(name, "refs/heads/", 11)) {
				if (rs->nbranch <
						LWS_ARRAY_SIZE(rs->branches))
					rs->branches[rs->nbranch++] = n;
			} else if (!strncmp(name, "refs/tags/", 10)) {
				if (rs->ntag < LWS_ARRAY_SIZE(rs->tags))
					rs->tags[rs->ntag++] = n;
			} else {
				if (rs->npost < LWS_ARRAY_SIZE(rs->posts))
					rs->posts[rs->npost++] = n;
			}
		}

		n = n->next;
	}
}

static long long
ref_time(const struct jg2_jn *ref, int tag)
{
	const struct jg2_jn *s = jg2_jn_obj_get(ref, "summary");
	const struct jg2_jn *sig, *t;

	if (!s)
		return 0;

	sig = jg2_jn_obj_get(s, tag ? "sig_tagger" : "sig_commit");
	if (!sig)
		sig = jg2_jn_obj_get(s, "sig_author");

	if (sig && (t = jg2_jn_obj_get(sig, "git_time")))
		return jg2_jn_ll(jg2_jn_obj_get(t, "time"));

	return jg2_jn_ll(jg2_jn_obj_get(s, "time"));
}

static int
emit_snapshot_links(struct jg2_srr *r, const char *tgt)
{
	char path[300];

	if (!(r->caps & 2))
		return 0;

	if (snprintf(path, sizeof(path), "%s-%s", r->reponame, tgt) >=
			(int)sizeof(path))
		return 0;

	if (HAP(r, "<a href='") ||
	    url_make(r, r->reponame, "snapshot", path, NULL, NULL, 0))
		return 1;

	return HAP(r, ".tar.gz'>gz</a>");
}

static int
render_branches(struct jg2_srr *r, const struct jg2_refsplit *rs, int count)
{
	size_t n;
	int shown = 0;

	if (!rs->nbranch)
		return 0;

	if (HAP(r, "<tr><td><table><tr><td class='heading' colspan='2'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Branch"),
			    strlen(i18n(r, "Branch"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Message"),
			    strlen(i18n(r, "Message"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Author"),
			    strlen(i18n(r, "Author"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Age"),
			    strlen(i18n(r, "Age"))) ||
	    HAP(r, "</td><td></td></tr>"))
		return 1;

	for (n = 0; n < rs->nbranch && shown < count; n++) {
		const struct jg2_jn *ref = rs->branches[n];
		const char *name = jg2_jn_str(jg2_jn_obj_get(ref, "name"));
		const struct jg2_jn *s = jg2_jn_obj_get(ref, "summary");
		const char *msg;

		if (!name)
			continue;

		shown++;
		msg = jg2_jn_str(jg2_jn_obj_get(s, "msg"));

		if (HAP(r, "<tr><td>") ||
		    emit_snapshot_links(r, name + 11) ||
		    HAP(r, "</td><td><a href='") ||
		    url_make(r, r->reponame, "tree", r->rpath, name + 11,
			     NULL, 0) ||
		    HAP(r, "'>") ||
		    ssr_esc(r, name + 11) ||
		    HAP(r, "</a></td><td>") ||
		    ssr_esc(r, msg) ||
		    HAP(r, "</td><td>") ||
		    emit_identity(r, jg2_jn_obj_get(s, "sig_author"), 16, 1) ||
		    HAP(r, "</td><td>") ||
		    emit_age_span(r, ref_time(ref, 0)) ||
		    HAP(r, "</td></tr>"))
			return 1;
	}

	if (shown == count && n < rs->nbranch) {
		if (HAP(r, "<tr><td colspan=5><a href='") ||
		    url_make(r, r->reponame, "branches", r->rpath, NULL, NULL,
			     0) ||
		    HAP(r, "'>[...]</a></td></tr>"))
			return 1;
	}

	return HAP(r, "</table></td></tr>");
}

static int
render_tags(struct jg2_srr *r, const struct jg2_refsplit *rs, int count)
{
	size_t n;
	int shown = 0;

	if (!rs->ntag)
		return 0;

	if (HAP(r, "<tr><td><table><tr><td class='heading' colspan='2'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Tag"), strlen(i18n(r, "Tag"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Message"),
			    strlen(i18n(r, "Message"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Author"),
			    strlen(i18n(r, "Author"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Age"),
			    strlen(i18n(r, "Age"))) ||
	    HAP(r, "</td><td></td></tr>"))
		return 1;

	for (n = 0; n < rs->ntag && shown < count; n++) {
		const struct jg2_jn *ref = rs->tags[n];
		const struct jg2_jn *s, *oidobj;
		const char *name = jg2_jn_str(jg2_jn_obj_get(ref, "name"));
		const char *msg, *oid;

		if (!name)
			continue;

		s = jg2_jn_obj_get(ref, "summary");
		if (!s)
			continue;

		/*
		 * Lightweight tags peel to commits with a plain oid;
		 * annotated tags expose the target via oid_tag.
		 */

		oidobj = jg2_jn_obj_get(s, "oid");
		if (!oidobj)
			oidobj = jg2_jn_obj_get(s, "oid_tag");
		oid = jg2_jn_str(jg2_jn_obj_get(oidobj, "oid"));
		if (!oid)
			continue;

		shown++;
		msg = jg2_jn_str(jg2_jn_obj_get(s, "msg_tag"));
		if (!msg)
			msg = jg2_jn_str(jg2_jn_obj_get(s, "msg"));

		if (HAP(r, "<tr><td>") ||
		    emit_snapshot_links(r, name + 10) ||
		    HAP(r, "</td><td><a href='") ||
		    url_make(r, r->reponame, "tree", r->rpath, NULL, oid, 0) ||
		    HAP(r, "'>") ||
		    ssr_esc(r, name + 10) ||
		    HAP(r, "</a></td><td>") ||
		    ssr_esc(r, msg) ||
		    HAP(r, "</td><td>") ||
		    emit_identity(r, jg2_jn_obj_get(s, "sig_tagger"), 16, 1) ||
		    HAP(r, "</td><td>") ||
		    emit_age_span(r, ref_time(ref, 1)) ||
		    HAP(r, "</td></tr>"))
			return 1;
	}

	if (shown == count && n < rs->ntag) {
		if (HAP(r, "<tr><td colspan=5><a href='") ||
		    url_make(r, r->reponame, "tags", r->rpath, NULL, NULL, 0) ||
		    HAP(r, "'>[...]</a></td></tr>"))
			return 1;
	}

	return HAP(r, "</table></td></tr>");
}

static int
render_log(struct jg2_srr *r, const struct jg2_jn *item, int count)
{
	const struct jg2_jn *list, *n, *next;
	int shown = 0;

	list = jg2_jn_obj_get(item, "log");
	if (!list || list->type != JG2_JN_ARR)
		return 0;

	if (HAP(r, "<table><tr><td class='heading'></td>"
		  "<td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Message"),
			    strlen(i18n(r, "Message"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Author"),
			    strlen(i18n(r, "Author"))) ||
	    HAP(r, "</td><td class='heading'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Age"),
			    strlen(i18n(r, "Age"))) ||
	    HAP(r, "</td><td class='heading'></td></tr>"))
		return 1;

	n = list->child;
	while (n && shown < count) {
		const struct jg2_jn *nameobj = jg2_jn_obj_get(n, "name");
		const struct jg2_jn *s = jg2_jn_obj_get(n, "summary");
		const char *oid = jg2_jn_str(jg2_jn_obj_get(nameobj, "oid"));
		const char *msg;

		if (oid && s) {
			msg = jg2_jn_str(jg2_jn_obj_get(s, "msg"));

			if (HAP(r, "<tr><td></td><td class='logmsg'>"
				  "<a href='") ||
			    url_make(r, r->reponame, "patch", NULL, NULL, oid,
				     0) ||
			    HAP(r, "'><img class='patch'></a> <a href='") ||
			    url_make(r, r->reponame, "commit", r->rpath, NULL,
				     oid, 0) ||
			    HAP(r, "'>") ||
			    ssr_esc(r, msg) ||
			    HAP(r, "</a> ") ||
			    emit_aliases(r, nameobj) ||
			    HAP(r, "</td><td>") ||
			    emit_identity(r, jg2_jn_obj_get(s, "sig_author"),
					  16, 1) ||
			    HAP(r, "</td><td>") ||
			    emit_age_span(r, jg2_jn_ll(
					jg2_jn_obj_get(s, "time"))) ||
			    HAP(r, "</td><td></td></tr>"))
				return 1;

			shown++;
		}

		n = n->next;
	}

	next = jg2_jn_obj_get(item, "next");
	if (next && shown == count) {
		const char *oid = jg2_jn_str(jg2_jn_obj_get(next, "oid"));

		if (oid) {
			if (HAP(r, "<tr><td colspan=5><a href='") ||
			    url_make(r, r->reponame, "log", r->rpath, NULL, oid,
				     0) ||
			    HAP(r, "'>next</a></td></tr>"))
				return 1;
		}
	}

	return HAP(r, "</table>");
}

static int
render_commit(struct jg2_srr *r, const struct jg2_jn *item)
{
	const struct jg2_jn *c = jg2_jn_obj_get(item, "commit");
	const struct jg2_jn *oidobj;
	const char *oid, *msg, *body, *diff;

	if (!c)
		return 0;

	oidobj = jg2_jn_obj_get(c, "oid");
	oid = jg2_jn_str(jg2_jn_obj_get(oidobj, "oid"));
	msg = jg2_jn_str(jg2_jn_obj_get(c, "msg"));
	body = jg2_jn_str(jg2_jn_obj_get(item, "body"));
	diff = jg2_jn_str(jg2_jn_obj_get(item, "diff"));

	if (HAP(r, "<div><table>"))
		return 1;

	if (HAP(r, "<tr><td>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Author"),
			    strlen(i18n(r, "Author"))) ||
	    HAP(r, "</td><td>") ||
	    emit_identity(r, jg2_jn_obj_get(c, "sig_author"), 16, 7) ||
	    HAP(r, "</td></tr>"))
		return 1;

	if (HAP(r, "<tr><td>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Committer"),
			    strlen(i18n(r, "Committer"))) ||
	    HAP(r, "</td><td>") ||
	    emit_identity(r, jg2_jn_obj_get(c, "sig_commit"), 16, 7) ||
	    HAP(r, "</td></tr>"))
		return 1;

	if (oid) {
		if (HAP(r, "<tr><td>") ||
		    jg2_hbuf_append(r->h, i18n(r, "Tree"),
				    strlen(i18n(r, "Tree"))) ||
		    HAP(r, "</td><td><a href='") ||
		    url_make(r, r->reponame, "tree", r->rpath, NULL, oid, 0) ||
		    HAP(r, "'>") ||
		    ssr_esc(r, oid) ||
		    HAP(r, "</a>&nbsp;&nbsp;<a href='") ||
		    url_make(r, r->reponame, "patch", r->rpath, NULL, oid, 0) ||
		    HAP(r, "'><img class='rawpatch'> ") ||
		    jg2_hbuf_append(r->h, i18n(r, "Raw Patch"),
				    strlen(i18n(r, "Raw Patch"))) ||
		    HAP(r, "</a></td></tr>"))
			return 1;
	}

	if (HAP(r, "<tr><td colspan=2>&nbsp;</td></tr>"
		  "<tr><td colspan=2 class='logtitle'>"))
		return 1;

	if (oid && (r->caps & 2)) {
		char tgt[128];

		if (snprintf(tgt, sizeof(tgt), "%s-%s", r->reponame, oid) <
				(int)sizeof(tgt)) {
			if (HAP(r, "<a href='") ||
			    url_make(r, r->reponame, "snapshot", tgt, NULL,
				     NULL, 0) ||
			    HAP(r, ".tar.gz'><img class='archive'></a> "))
				return 1;
		}
	}

	if (ssr_esc(r, msg) ||
	    HAP(r, " ") ||
	    emit_aliases(r, oidobj) ||
	    HAP(r, "</td></tr>"))
		return 1;

	if (body) {
		if (HAP(r, "<tr><td colspan=2><pre class='logbody'>") ||
		    ssr_esc(r, body) ||
		    HAP(r, "</pre></td></tr>"))
			return 1;
	}

	if (HAP(r, "</table></div>"))
		return 1;

	if (diff) {
		if (HAP(r, "<div><pre><main role='main'>"
			  "<code id='do-hljs' class=\"diff\">"))
			return 1;

		/*
		 * Diff markup highlighting (adds / removes / hunks / file
		 * metadata lines) server-side; falls back to escaped text.
		 */

#if defined(JG2_HAVE_HL_DIFF)
		if (hl_emit(r->h, lws_hl_lang_diff(), diff, strlen(diff)) &&
		    ssr_esc(r, diff))
			return 1;
#else
		if (ssr_esc(r, diff))
			return 1;
#endif

		if (HAP(r, "</code></main></pre></div>"))
			return 1;
	}

	return 0;
}

/* emit the line-number column for a blob */

static int
emit_line_numbers(struct jg2_srr *r, const char *blob, size_t len)
{
	size_t n;
	int line = 1, fresh = 1;

	if (HAP(r, "<div id='jglinenumbers' class='jglinenumbers'>"))
		return 1;

	for (n = 0; n < len; n++) {
		if (fresh) {
			if (jg2_hbuf_printf(r->h,
					"<a id='n%d' href='#n%d'>%d\n",
					line, line, line))
				return 1;
			line++;
			fresh = 0;
		}

		if (blob[n] == '\n')
			fresh = 1;
	}

	return HAP(r, "</div>");
}

static size_t
blob_line_count(const char *blob, size_t len)
{
	size_t n, count = 0;

	if (!len)
		return 0;

	for (n = 0; n < len; n++)
		if (blob[n] == '\n')
			count++;

	if (blob[len - 1] != '\n')
		count++;

	return count;
}

/*
 * Blame decoration: map final line numbers to hunks so the code can carry
 * per-line shading and a hover title identifying the commit.
 */

struct jg2_blamemap {
	const struct jg2_jn *hunks;
	int16_t *line2hunk;
	size_t lines;
};

static void
blamemap_build(const struct jg2_jn *blame, struct jg2_blamemap *bm,
	       size_t lines, struct lwsac **ac)
{
	const struct jg2_jn *h;
	size_t hn = 0;

	memset(bm, 0, sizeof(*bm));
	bm->hunks = blame;
	bm->lines = lines;

	if (!blame || !lines)
		return;

	bm->line2hunk = lwsac_use(ac, sizeof(int16_t) * lines, 0);
	if (!bm->line2hunk)
		return;

	h = blame->child;
	while (h) {
		const struct jg2_jn *ranges = jg2_jn_obj_get(h, "ranges");
		const struct jg2_jn *rg;

		if (ranges)
			for (rg = ranges->child; rg; rg = rg->next) {
				long long f = jg2_jn_ll(jg2_jn_obj_get(rg, "f"));
				long long l = jg2_jn_ll(jg2_jn_obj_get(rg, "l"));
				long long k;

				for (k = 0; k < l; k++) {
					size_t ln = (size_t)(f + k - 1);

					if (ln < lines)
						bm->line2hunk[ln] = (int16_t)(hn + 1);
				}
			}

		hn++;
		h = h->next;
	}
}

/* the hunk list index for a 1-based line2hunk entry */

static const struct jg2_jn *
blamemap_hunk(const struct jg2_jn *blame, int idx1)
{
	const struct jg2_jn *h = blame ? blame->child : NULL;
	int n = 0;

	while (h && n < idx1 - 1) {
		h = h->next;
		n++;
	}

	return h;
}

/* original-file line number for a 1-based final line, via the ranges */

static long long
blamemap_orig_line(const struct jg2_jn *hk, size_t line1)
{
	const struct jg2_jn *rg;

	if (!hk || !line1)
		return 0;

	rg = jg2_jn_obj_get(hk, "ranges");
	if (rg)
		for (rg = rg->child; rg; rg = rg->next) {
			long long f = jg2_jn_ll(jg2_jn_obj_get(rg, "f"));
			long long l = jg2_jn_ll(jg2_jn_obj_get(rg, "l"));

			if (line1 >= (size_t)f && line1 < (size_t)(f + l))
				return jg2_jn_ll(jg2_jn_obj_get(rg, "o"));
		}

	return 0;
}

/*
 * The no-JS blame popup: emitted after the code table, unhidden purely by
 * css :target when a blame group anchor pointing at #blp-N is clicked.
 */

static int
emit_blame_panel(struct jg2_srr *r, struct jg2_hbuf *pop, int g,
		 const struct jg2_jn *hk, size_t gline)
{
	const struct jg2_jn *sig = jg2_jn_obj_get(hk, "sig_final");
	const char *fo = jg2_jn_str(jg2_jn_obj_get(
				jg2_jn_obj_get(hk, "final_oid"), "oid"));
	const char *oo = jg2_jn_str(jg2_jn_obj_get(
				jg2_jn_obj_get(hk, "orig_oid"), "oid"));
	const char *lg = jg2_jn_str(jg2_jn_obj_get(hk, "log_final"));
	const char *op = jg2_jn_str(jg2_jn_obj_get(hk, "op"));
	struct jg2_hbuf *save = r->h;
	long long oline;
	int ret = 1;

	if (!fo || !sig)
		return 0;

	if (jg2_hbuf_printf(pop, "<div class=\"blpop\" id=\"blp-%d\">"
			    "<div class=\"blpop-x\">"
			    "<a href=\"#\">&times;</a></div>"
			    "<table><tr><td class=\"blpop-av\">", g))
		goto bail;

	/* identity emits through r->h: aim it at the popup buffer */

	r->h = pop;
	if (emit_identity(r, sig, 64, 7))
		goto bail;

	if (HAP(r, "</td><td class=\"blpop-info\">"
		  "<div class=\"blpop-log\"><a href=\"") ||
	    url_make(r, r->reponame, "commit", NULL, NULL, fo, 0) ||
	    HAP(r, "\">") ||
	    ssr_esc(r, lg) ||
	    HAP(r, "</a></div>"
		  "<div class=\"blpop-oid\"><a href=\"") ||
	    url_make(r, r->reponame, "commit", NULL, NULL, fo, 0) ||
	    jg2_hbuf_printf(r->h, "\">%s</a> <a href=\"", fo) ||
	    url_make(r, r->reponame, "patch", NULL, NULL, fo, 0) ||
	    HAP(r, "\">patch</a></div>"))
		goto bail;

	/*
	 * The "blame at the commit this came from" link, like the old js
	 * blameotron devolve: only meaningful when it would go somewhere
	 * else than we already are.
	 */

	oline = blamemap_orig_line(hk, gline);

	if (oo && oline && (!r->qid || strcmp(oo, r->qid))) {
		if (HAP(r, "<div class=\"blpop-old\">"
			  "<a href=\"") ||
		    url_make(r, r->reponame, "blame",
			     op && op[0] ? op : r->rpath, NULL, oo, 0) ||
		    jg2_hbuf_printf(r->h, "#n%lld\">", oline) ||
		    ssr_esc(r, oo) ||
		    HAP(r, "</a></div>"))
			goto bail;
	}

	if (HAP(r, "</td></tr></table></div>"))
		goto bail;

	ret = 0;

bail:
	r->h = save;

	return ret;
}

static int
emit_code_blamed(struct jg2_srr *r, const char *blob, size_t len,
		 const struct jg2_blamemap *bm, const char *blobname,
		 struct jg2_hbuf *pop)
{
	size_t pos = 0, gstart = 0, lineno = 0, gline = 0;
	int cur = -1, g = 0;

#if defined(LWS_WITH_HL)
	lws_hl_ctx_t hlctx;
	lws_hl_html_t hlhtml;
	struct hl_sink hls;
	const lws_hl_ops_t *lang = hl_lang_for(blobname);
	int hl_ok = 0;

	if (lang) {
		hls.h = r->h;
		hls.fail = 0;

		if (!lws_hl_html_construct(&hlhtml, hl_write_cb, &hls,
					   NULL) &&
		    !lws_hl_construct(&hlctx, lang, lws_hl_html_token,
				      &hlhtml))
			hl_ok = 1;
	}
#endif

	while (pos <= len) {
		size_t le = pos;
		int h, closing;

		while (le < len && blob[le] != '\n')
			le++;

		h = (lineno < bm->lines) ? bm->line2hunk[lineno] : 0;

		if (cur == -1) {
			cur = h;
			gstart = pos;
			gline = lineno + 1;
		}

		/*
		 * Close the group when the hunk changes, or at the last
		 * line.  The group spans [gstart, gend) including the
		 * trailing newline.
		 */

		closing = (h != cur) || (le == len);

		if (closing) {
			/*
			 * The group spans [gstart, gend).  When we are
			 * closing because the hunk changed on this line, the
			 * current line belongs to the NEXT group: end at pos,
			 * the start of the current line (ending at le would
			 * wrongly include this line's text, since le is the
			 * index of this line's newline).  Otherwise (end of
			 * file) include the final line including its newline,
			 * if any.
			 */

			size_t gend = (h != cur) ? pos :
					((le < len) ? le + 1 : len);
			const struct jg2_jn *hk;

			if (gend > gstart) {

			if (cur > 0 && (hk = blamemap_hunk(bm->hunks, cur))) {
				const struct jg2_jn *sig =
					jg2_jn_obj_get(hk, "sig_final");
				const char *nm = jg2_jn_str(
						jg2_jn_obj_get(sig, "name"));
				const char *lg = jg2_jn_str(
						jg2_jn_obj_get(hk, "log_final"));
				const char *fo = jg2_jn_str(jg2_jn_obj_get(
					jg2_jn_obj_get(hk, "final_oid"), "oid"));
				const struct jg2_jn *gt = jg2_jn_obj_get(
					jg2_jn_obj_get(sig, "git_time"),
					"time");

				/*
				 * The group is an anchor to its css
				 * :target popup, carrying the hunk info
				 * and the link to the commit.
				 */

				g++;

				if (jg2_hbuf_printf(r->h,
						"<a class='blm' href='#blp-%d'>"
						"<span class='bl-%d' title=\"",
						g, (cur - 1) & 7) ||
				    ssr_esc_attr(r, nm) ||
				    HAP(r, ", ") ||
				    emit_age_plain(r, gt ? jg2_jn_ll(gt) : 0) ||
				    HAP(r, " &mdash; ") ||
				    ssr_esc_attr(r, lg) ||
				    jg2_hbuf_printf(r->h, " (%s)\">", fo ? fo : ""))
					return 1;
			}

			/*
			 * The group text: highlighted through the
			 * persistent tokenizer when possible (the groups
			 * partition the file, so the tokenizer sees exactly
			 * the original byte stream), else escaped.
			 */

#if defined(LWS_WITH_HL)
			if (hl_ok) {
				const uint8_t *hp = (const uint8_t *)(blob + gstart);
				size_t hl = gend - gstart;

				while (hl && !hls.fail) {
					size_t was = hl;

					if (lws_hl_parse(&hlctx, &hp, &hl))
						break;
					if (hl == was)
						break;
				}
				if (hls.fail)
					return 1;
			} else
#endif
			{
				if (ssr_esc_len(r, blob + gstart,
						gend - gstart))
					return 1;
			}

			if (cur > 0 && blamemap_hunk(bm->hunks, cur)) {
				if (HAP(r, "</span></a>"))
					return 1;

				/* the css popup for this group */

				if (emit_blame_panel(r, pop, g,
						     blamemap_hunk(bm->hunks, cur),
						     gline))
					return 1;
			}

			}

			if (h != cur) {
				/*
				 * Reprocess this line as the first line of
				 * the new group, without consuming it.
				 */

				cur = h;
				gstart = pos;
				gline = lineno + 1;

				continue;
			}

			cur = -1;
		}

		lineno++;

		if (le == len)
			break;

		pos = le + 1;
	}

#if defined(LWS_WITH_HL)
	if (hl_ok) {
		if (lws_hl_finish(&hlctx) || lws_hl_html_close(&hlhtml))
			return 1;
	}
#endif

	return 0;
}

static int
render_tree(struct jg2_srr *r, const struct jg2_jn * const *items,
	    size_t nitems)
{
	const struct jg2_jn *item0 = items[0];
	const struct jg2_jn *blobitem = NULL;
	const struct jg2_jn *blameitem = NULL;
	const struct jg2_jn *tree;
	const char *blob, *blobname, *bloblink;
	size_t blob_len = 0;
	size_t bi, blobidx = 0;

	tree = jg2_jn_obj_get(item0, "tree");

	/*
	 * Work out which item (if any) carries the blob: after a directory
	 * listing it is the second item, else the first.
	 */

	bi = tree ? 1 : 0;

	if (nitems > bi &&
	    (jg2_jn_obj_get(items[bi], "blob") ||
	     jg2_jn_obj_get(items[bi], "bloblink"))) {
		blobitem = items[bi];
		blobidx = bi;
	}

	/* blame / contrib info rides in the item after the blob */

	if (blobitem && blobidx + 1 < nitems &&
	    jg2_jn_obj_get(items[blobidx + 1], "blame"))
		blameitem = items[blobidx + 1];

	/* directory listing */

	if (tree && tree->type == JG2_JN_ARR) {
		const struct jg2_jn *n = tree->child;

		if (HAP(r, "<div id=\"sai_sticky\"></div>"
			  "<div class='jg2-tree'><pre><table><tr>"
			  "<td class='heading'>") ||
		    jg2_hbuf_append(r->h, i18n(r, "Mode"),
				    strlen(i18n(r, "Mode"))) ||
		    HAP(r, "</td><td class='headingr'>") ||
		    jg2_hbuf_append(r->h, i18n(r, "Size"),
				    strlen(i18n(r, "Size"))) ||
		    HAP(r, "</td><td class='heading'>") ||
		    jg2_hbuf_append(r->h, i18n(r, "Name"),
				    strlen(i18n(r, "Name"))) ||
		    HAP(r, "</td></tr>"))
			return 1;

		while (n) {
			const char *name = jg2_jn_str(jg2_jn_obj_get(n, "name"));
			const char *mode = jg2_jn_str(jg2_jn_obj_get(n, "mode"));
			const struct jg2_jn *szn = jg2_jn_obj_get(n, "size");
			long long mode_ll = mode ? atoll(mode) : 0;
			char childpath[512];

			if (name) {
				if (r->rpath && r->rpath[0])
					snprintf(childpath, sizeof(childpath),
						 "%s/%s", r->rpath, name);
				else
					snprintf(childpath, sizeof(childpath),
						 "%s", name);

				if ((mode_ll & 0xe000) == 0xe000) {
					if (HAP(r, "<tr><td>&nbsp</td>"
						  "<td>&nbsp</td>"
						  "<td class='dl-dir'>"
						  "<img class='submodule'>&nbsp;")
					    || ssr_esc(r, name) ||
					    HAP(r, "</td></tr>"))
						return 1;
				} else if (mode_ll & 16384) {
					if (HAP(r, "<tr><td>-r-xr-xr-x</td>"
						  "<td>&nbsp</td>"
						  "<td class='dl-dir'>"
						  "<img class='folder'>&nbsp;"
						  "<a class='noline' href='") ||
					    url_make(r, r->reponame, r->mode,
						     childpath, NULL, NULL,
						     UQ_BRANCH | UQ_ID |
						     UQ_OFS) ||
					    HAP(r, "'>") ||
					    ssr_esc(r, name) ||
					    HAP(r, "</a></td></tr>"))
						return 1;
				} else {
					if (jg2_hbuf_printf(r->h,
						"<tr><td>-rw-r--r--</td>"
						"<td class='r'>%lld</td>"
						"<td class='dl-file'>"
						"<a class='noline' href='",
						jg2_jn_ll(szn)) ||
					    url_make(r, r->reponame, r->mode,
						     childpath, NULL, NULL,
						     UQ_BRANCH | UQ_ID |
						     UQ_OFS) ||
					    HAP(r, "'>") ||
					    ssr_esc(r, name) ||
					    HAP(r, "</a></td></tr>"))
						return 1;
				}
			}

			n = n->next;
		}

		if (HAP(r, "</table></pre></div>"))
			return 1;
	}

	if (!blobitem)
		return 0;

	blob = jg2_jn_str2(jg2_jn_obj_get(blobitem, "blob"), &blob_len);
	blobname = jg2_jn_str(jg2_jn_obj_get(blobitem, "blobname"));
	bloblink = jg2_jn_str(jg2_jn_obj_get(blobitem, "bloblink"));

	if (!blobname || !blobname[0])
		blobname = r->rpath;

	/* markdown documents */

	if (blob && blobname) {
		size_t nl = strlen(blobname);
		int is_md = (nl > 3 && !strcmp(blobname + nl - 3, ".md")) ||
			    (nl > 4 && !strcmp(blobname + nl - 4, ".mkd"));

		if (is_md || (r->caps & 4)) {

			/* doc_dir for relative link resolution */

			r->doc_dir[0] = '\0';
			if (r->rpath) {
				const char *sl = strrchr(r->rpath, '/');

				if (sl) {
					size_t dl = (size_t)(sl - r->rpath) + 1;

					if (dl < sizeof(r->doc_dir)) {
						memcpy(r->doc_dir, r->rpath,
						       dl);
						r->doc_dir[dl] = '\0';
					}
				}
			}

			if (r->caps & 4) {
				/* blog article: % title / date header */

				size_t n = 0, ls = 0, pc = 0;

				if (HAP(r, "<div class='blog-article-content' "
					  "id='do-showdown'>"))
					return 1;

				while (n <= blob_len && pc < 3) {
					if (n == blob_len ||
					    blob[n] == '\n') {
						size_t ll = n - ls;

						if (ll && blob[ls] == '%') {
							if (pc == 0) {
								if (HAP(r, "<span class='blogtitle'>") ||
								    ssr_esc_len(r, blob + ls + 1,
										ll - 1) ||
								    HAP(r, "</span><br>"))
									return 1;
							} else if (pc == 2) {
								if (HAP(r, "<span class='blogdate'>") ||
								    ssr_esc_len(r, blob + ls + 1,
										ll - 1) ||
								    HAP(r, "</span><p>"))
									return 1;
							}
							pc++;
							/* skip the rest of
							 * the % header lines
							 * from markdown */
						}
						ls = n + 1;
					}
					n++;
				}

				/* markdown the remainder */

				if (emit_markdown(r, blob + ls, blob_len - ls))
					return 1;

				return HAP(r, "</div>");
			}

			if (HAP(r, "<div class='inline'>"
				  "<div class='inline-title'>") ||
			    ssr_esc(r, blobname) ||
			    HAP(r, "</div><table><tr>"
				  "<td class='doc' id='do-showdown'>") ||
			    emit_markdown(r, blob, blob_len) ||
			    HAP(r, "</td></tr></table></div>"))
				return 1;

			return 0;
		}
	}

	/* binary blob: image or download link */

	if (bloblink) {
		if (HAP(r, "<div class='inline'><div class='inline-title'>") ||
		    ssr_esc(r, blobname) ||
		    HAP(r, "</div><table><tr><td class='doc'>"))
			return 1;

		{
			size_t ll = strlen(bloblink);

			if ((ll > 4 &&
			     (!strcasecmp(bloblink + ll - 4, ".png") ||
			      !strcasecmp(bloblink + ll - 4, ".jpg") ||
			      !strcasecmp(bloblink + ll - 4, ".ico"))) ||
			    (ll > 5 &&
			     !strcasecmp(bloblink + ll - 5, ".jpeg"))) {
				if (HAP(r, "<img src=\"") ||
				    ssr_esc_attr(r, bloblink) ||
				    HAP(r, "\">"))
					return 1;
			} else {
				if (HAP(r, "<a href=\"") ||
				    ssr_esc_attr(r, bloblink) ||
				    HAP(r, "\" download>") ||
				    jg2_hbuf_append(r->h,
						    i18n(r, "Download"),
						    strlen(i18n(r,
							     "Download"))) ||
				    HAP(r, "</a>"))
					return 1;
			}
		}

		return HAP(r, "</td></tr></table></div>");
	}

	/* plain code file */

	if (!blob)
		return 0;

	/* code table: contrib strip at the top when blame info is present */

	if (HAP(r, "<table>"))
		return 1;

	if (blameitem) {
		const struct jg2_jn *contrib = jg2_jn_obj_get(blameitem,
							      "contrib");
		const struct jg2_jn *blame = jg2_jn_obj_get(blameitem,
							    "blame");
		const struct jg2_jn *cn;
		int count = 0;

		if (contrib && blame) {
			if (HAP(r, "<tr><td colspan='2'><table><tr>"))
				return 1;

			for (cn = contrib->child;
			     cn && count < 20; cn = cn->next) {
				long long ord = jg2_jn_ll(
						jg2_jn_obj_get(cn, "o"));
				const struct jg2_jn *hk = blamemap_hunk(blame,
							       (int)ord + 1);

				if (HAP(r, "<td>") ||
				    emit_identity(r, jg2_jn_obj_get(hk,
							"sig_final"), 24, 0) ||
				    HAP(r, "</td>"))
					return 1;

				count++;
			}

			if (HAP(r, "</tr></table></td></tr>"))
				return 1;
		}
	}

	if (HAP(r, "<tr><td><pre><code>"))
		return 1;

	if (emit_line_numbers(r, blob, blob_len))
		return 1;

	if (HAP(r, "</code></pre></td><td class='doc'>"
		  "<pre><code id=\"do-hljs\">"))
		return 1;

	{
		struct jg2_hbuf pop;

		memset(&pop, 0, sizeof(pop));

		if (blameitem) {
			struct jg2_blamemap bm;
			struct lwsac *ac = NULL;
			int ret;

			blamemap_build(jg2_jn_obj_get(blameitem, "blame"), &bm,
				       blob_line_count(blob, blob_len), &ac);
			ret = emit_code_blamed(r, blob, blob_len, &bm,
					       blobname, &pop);
			lwsac_free(&ac);

			if (ret) {
				free(pop.buf);

				return 1;
			}
		} else {
			/*
			 * Plain code view: server-side highlighting for
			 * languages we know, escaped text for everything
			 * else.
			 */

			if (hl_emit_file(r->h, blobname, blob, blob_len) &&
			    ssr_esc_len(r, blob, blob_len))
				return 1;
		}

		if (HAP(r, "</code></pre></td></tr></table>"))
			return 1;

		/*
		 * The css-only blame popups, emitted after the code table
		 * (not inside the code element, where they would be
		 * invalid nesting).
		 */

		if (pop.len && jg2_hbuf_append(r->h, pop.buf, pop.len)) {
			free(pop.buf);

			return 1;
		}
		free(pop.buf);
	}

	return 0;
}

static int
render_bloglist(struct jg2_srr *r, const struct jg2_jn *item)
{
	struct jg2_refsplit rs;
	size_t n;

	reflist_split(item, &rs);

	if (!rs.npost)
		return 0;

	if (HAP(r, "<div class='jg2-bloglist'>"))
		return 1;

	for (n = 0; n < rs.npost; n++) {
		const struct jg2_jn *post = rs.posts[n];
		const char *name = jg2_jn_str(jg2_jn_obj_get(post, "name"));
		const char *title = jg2_jn_str(jg2_jn_obj_get(post, "title"));
		const char *summary = jg2_jn_str2(
				jg2_jn_obj_get(post, "summary"), NULL);

		if (!name)
			continue;

		if (HAP(r, "<a class='blog-post-link' href=\"") ||
		    url_make(r, r->reponame, NULL, name, NULL, NULL,
			     UQ_BRANCH | UQ_ID) ||
		    HAP(r, "\"><div class='blog-post-summary'>"
			  "<span class='blogtitle'>") ||
		    ssr_esc(r, title) ||
		    HAP(r, "</span><br>"))
			return 1;

		{
			const char *sl = strrchr(name, '/');

			if (sl && sl != name) {
				if (HAP(r, "<span class='blogdate'>") ||
				    ssr_esc_len(r, name, (size_t)(sl - name)) ||
				    HAP(r, "</span><p>"))
					return 1;
			}
		}

		r->doc_dir[0] = '\0';
		{
			const char *sl = strrchr(name, '/');

			if (sl) {
				size_t dl = (size_t)(sl - name) + 1;

				if (dl < sizeof(r->doc_dir)) {
					memcpy(r->doc_dir, name, dl);
					r->doc_dir[dl] = '\0';
				}
			}
		}

		if (summary && summary[0]) {
			if (HAP(r, "<div class='blogsummary'>") ||
			    emit_markdown(r, summary, strlen(summary)) ||
			    HAP(r, "...</div>"))
				return 1;
		}

		if (HAP(r, "</div></a><br>"))
			return 1;
	}

	return HAP(r, "</div>");
}

static int
render_search_files(struct jg2_srr *r, const struct jg2_jn *item)
{
	const struct jg2_jn *list, *n;
	int rows = 0;

	list = jg2_jn_obj_get(item, "search");

	if (HAP(r, "<tr><td class='searchfiles'><main role=\"main\">"
		  "<table>"))
		return 1;

	if (list && list->type == JG2_JN_ARR) {
		n = list->child;
		while (n) {
			const char *fp = jg2_jn_str(jg2_jn_obj_get(n, "fp"));
			const struct jg2_jn *m = jg2_jn_obj_get(n, "matches");

			if (fp) {
				if (HAP(r, "<tr><td class='rpathinfo'>") ||
				    jg2_hbuf_printf(r->h, "%lld",
						    jg2_jn_ll(m)) ||
				    HAP(r, "</td><td class='path'>"
					  "<a href='") ||
				    url_make(r, r->reponame, "tree", fp,
					     NULL, NULL,
					     UQ_BRANCH | UQ_SEARCH) ||
				    HAP(r, "'>") ||
				    ssr_esc(r, fp) ||
				    HAP(r, "</a></td></tr>"))
					return 1;

				rows++;
			}

			n = n->next;
		}
	}

	if (!rows) {
		if (HAP(r, "<tr><td><span class='info'>No results"
			  "</span></td></tr>"))
			return 1;
	}

	return HAP(r, "</table></main></td><td>");
}

/* ----------------------------------------------------------------------
 * page chrome
 */

static int
emit_chrome(struct jg2_srr *r, const struct jg2_jn *alias_oid)
{
	const char *sel_summary = "", *sel_log = "", *sel_blame = "",
		   *sel_tree = "";
	const char *mo = strcmp(r->mode, "blame") ? "tree" : "blame";

	if (!strcmp(r->mode, "summary"))
		sel_summary = " class=\"selected\" ";
	else if (!strcmp(r->mode, "log"))
		sel_log = " class=\"selected\" ";
	else if (!strcmp(r->mode, "blame"))
		sel_blame = " class=\"selected\" ";
	else if (!strcmp(r->mode, "tree"))
		sel_tree = " class=\"selected\" ";

	if (r->caps & 8) {
		if (HAP(r, "<div class='warning-banner'>System overloaded: "
			  "Blame disabled, showing tree instead.</div>"))
			return 1;
	}

	if (HAP(r, "<tr class='repobar'><td class='repobar'>"
		  "<div class='repobar'><table><tr><td>"
		  "<span class='reponame'><a href='") ||
	    url_make(r, NULL, NULL, NULL, NULL, NULL, 0) ||
	    HAP(r, "'><img class='repolist'></a>&nbsp;&nbsp;"
		  "<span id='lws-user-info' class='user-info'></span>"
		  "&nbsp;&nbsp;<a href=\"") ||
	    url_make(r, r->reponame, "summary", NULL, NULL, NULL, 0) ||
	    HAP(r, "\">") ||
	    ssr_esc(r, r->reponame) ||
	    HAP(r, "&nbsp;&nbsp;</span></td><td class='tight'><table>"))
		return 1;

	if (r->repo.desc) {
		if (HAP(r, "<tr><td class='tight info' colspan='2'>"
			  "<img class='info'>&nbsp;") ||
		    ssr_esc(r, r->repo.desc) ||
		    HAP(r, "</td></tr>"))
			return 1;
	}

	if (HAP(r, "<tr>"))
		return 1;

	if (r->repo.owner) {
		if (HAP(r, "<td class='tight'> ") ||
		    emit_identity(r, r->repo.owner, 16, 1) ||
		    HAP(r, "</td>"))
			return 1;
	} else if (HAP(r, "<td></td>"))
		return 1;

	if (r->repo.url) {
		if (HAP(r, "<td class='tight'> "
			  "<i>git clone ") ||
		    ssr_esc(r, r->repo.url) ||
		    HAP(r, "</i></td>"))
			return 1;
	} else if (HAP(r, "<td></td>"))
		return 1;

	if (HAP(r, "</tr></table></td></tr></tbody></table></div></td>"
		  "<td class='rt'><div class='search'>"
		  "<table><tr><td rowspan='2'><img class='eyeglass'></td>"
		  "<td class='searchboxtitle'>Fulltext search<br>"
		  "<form action='") ||
	    url_make(r, r->reponame, "search", NULL, NULL, NULL, 0) ||
	    HAP(r, "' method='get' class='ssr-searchform'>"
		  "<input type='text' id='gohsearch' name='q' maxlength='80' "
		  "value='") ||
	    ssr_esc_attr(r, r->qsearch) ||
	    HAP(r, "'></form></td></tr></table></div></td></tr>"
		  "<tr class='tabbar'><td class='tabbar'><div class='tabbar'>"
		  "<div class='tabs'><ul>"))
		return 1;

	if (HAP(r, "<li ") || jg2_hbuf_append(r->h, sel_summary,
					      strlen(sel_summary)) ||
	    HAP(r, "><a href=\"") ||
	    url_make(r, r->reponame, "summary", NULL, NULL, NULL, 0) ||
	    HAP(r, "\"><img class='summary'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Summary"),
			    strlen(i18n(r, "Summary"))) ||
	    HAP(r, "</a></li>"))
		return 1;

	if (HAP(r, "<li ") || jg2_hbuf_append(r->h, sel_log, strlen(sel_log)) ||
	    HAP(r, "><a href=\"") ||
	    url_make(r, r->reponame, "log", r->rpath, NULL, NULL,
		     UQ_BRANCH | UQ_ID | UQ_OFS) ||
	    HAP(r, "\"><img class='commits'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Log"), strlen(i18n(r, "Log"))) ||
	    HAP(r, "</a></li>"))
		return 1;

	if (HAP(r, "<li ") || jg2_hbuf_append(r->h, sel_tree,
					      strlen(sel_tree)) ||
	    HAP(r, "><a href=\"") ||
	    url_make(r, r->reponame, "tree", r->rpath, NULL, NULL,
		     UQ_BRANCH | UQ_ID | UQ_OFS) ||
	    HAP(r, "\"><img class='tree'>") ||
	    jg2_hbuf_append(r->h, i18n(r, "Tree"),
			    strlen(i18n(r, "Tree"))) ||
	    HAP(r, "</a></li>"))
		return 1;

	if (r->caps & 1) {
		if (HAP(r, "<li ") ||
		    jg2_hbuf_append(r->h, sel_blame, strlen(sel_blame)) ||
		    HAP(r, "><a href=\"") ||
		    url_make(r, r->reponame, "blame", r->rpath, NULL, NULL,
			     UQ_BRANCH | UQ_ID | UQ_OFS) ||
		    HAP(r, "\"><img class='blame'>") ||
		    jg2_hbuf_append(r->h, i18n(r, "Blame"),
				    strlen(i18n(r, "Blame"))) ||
		    HAP(r, "</a></li>"))
			return 1;
	}

	if (HAP(r, "</ul></div>&nbsp;"
		  "<div id=\"sai_sticky\" class='sai_sticky'></div>"
		  "<div class='rpathname'>"))
		return 1;

	if (alias_oid && emit_aliases(r, alias_oid))
		return 1;

	if (r->rpath && r->rpath[0]) {
		char agg[512];
		size_t alen = 0;
		const char *e = r->rpath;

		if (HAP(r, "<a href=\"") ||
		    url_make(r, r->reponame, mo, "", NULL, NULL,
			     UQ_BRANCH | UQ_ID | UQ_OFS) ||
		    HAP(r, "\">") ||
		    jg2_hbuf_append(r->h, i18n(r, "root"),
				    strlen(i18n(r, "root"))) ||
		    HAP(r, "</a><span class='repopath'>"))
			return 1;

		while (*e) {
			const char *sl = strchr(e, '/');
			size_t slen = sl ? (size_t)(sl - e) : strlen(e);

			if (!slen)
				goto next;

			if (alen && alen < sizeof(agg) - 1)
				agg[alen++] = '/';
			if (alen + slen >= sizeof(agg) - 1)
				break;
			memcpy(agg + alen, e, slen);
			alen += slen;
			agg[alen] = '\0';

			if (!sl) {
				if (HAP(r, " / ") ||
				    ssr_esc(r, e))
					return 1;
			} else {
				char seg[256];

				if (slen < sizeof(seg)) {
					memcpy(seg, e, slen);
					seg[slen] = '\0';

					if (HAP(r, " / <a href=\"") ||
					    url_make(r, r->reponame, mo, agg,
						     NULL, NULL,
						     UQ_BRANCH | UQ_ID |
						     UQ_OFS) ||
					    HAP(r, "\">") ||
					    ssr_esc(r, seg) ||
					    HAP(r, "</a>"))
						return 1;
				}
			}

			if (!sl)
				break;
next:
			e = sl ? sl + 1 : e + slen;
		}

		if (HAP(r, "</span>"))
			return 1;
	}

	return HAP(r, "</div></div></td></tr>");
}

/* ----------------------------------------------------------------------
 * page assembly
 */

static int
is_known_mode(const char *m)
{
	static const char *known[] = { "tree", "blame", "log", "commit",
		"tags", "branches", "summary", "search", "blog" };
	size_t n;

	for (n = 0; n < LWS_ARRAY_SIZE(known); n++)
		if (!strcmp(m, known[n]))
			return 1;

	return 0;
}

static int
render_page(struct jg2_srr *r, const struct jg2_jn *items)
{
	const struct jg2_jn *item[3];
	size_t nitems = 0;
	const struct jg2_jn *it = items;

	while (it && nitems < 3) {
		item[nitems++] = it;
		it = it->next;
	}

	if (HAP(r, "<table class='repobar'><tbody class='repobar'>"))
		return 1;

	if (!nitems) {
		/*
		 * Nothing captured (eg a search with no index yet): still
		 * give the page its chrome rather than an empty region.
		 */

		if (r->reponame && r->reponame[0] && !(r->caps & 4)) {
			if (emit_chrome(r, NULL))
				return 1;
		}

		if (HAP(r, "<tr><td><main role=\"main\">"
			  "<span class='info'>No results</span>"
			  "</main></td></tr></table>"))
			return 1;

		return 0;
	}

	/* error item? */

	if (nitems && jg2_jn_obj_get(item[0], "error")) {
		const char *e = jg2_jn_str(jg2_jn_obj_get(item[0], "error"));

		if (HAP(r, "<tr><td><span class='error'>"
			  "<img class='warning'>") ||
		    ssr_esc(r, e) ||
		    HAP(r, "</span></td></tr></table>"))
			return 1;

		return 0;
	}

	/* repo index page */

	if (!r->reponame || !r->reponame[0]) {
		if (HAP(r, "<tr><td><div id='lws-user-info' "
			  "class='user-info'></div></td></tr>"
			  "<tr><td class='repolist'>") ||
		    render_repolist(r, item[0]) ||
		    HAP(r, "</td></tr></table>"))
			return 1;

		return 0;
	}

	/* work out the alias source for the tab bar */

	{
		const struct jg2_jn *alias_oid = NULL;
		const struct jg2_jn *c, *l;

		if (!strcmp(r->mode, "commit")) {
			c = jg2_jn_obj_get(item[0], "commit");
			alias_oid = jg2_jn_obj_get(c, "oid");
		} else if (!strcmp(r->mode, "log")) {
			l = jg2_jn_obj_get(item[0], "log");
			if (l && l->child)
				alias_oid = jg2_jn_obj_get(l->child, "name");
		} else if (!strcmp(r->mode, "tree") || !strcmp(r->mode, "blame")) {
			alias_oid = jg2_jn_obj_get(item[0], "oid");
		}

		if (!(r->caps & 4) && emit_chrome(r, alias_oid))
			return 1;
	}

	/* content */

	if (r->caps & 4) {
		/* blog mode pages have no chrome */

		if (!strcmp(r->mode, "blog") || !r->mode[0]) {
			if (HAP(r, "<tr><td><main role=\"main\">") ||
			    render_bloglist(r, item[0]) ||
			    HAP(r, "</main></td></tr></table>"))
				return 1;

			return 0;
		}
	}

	if (!strcmp(r->mode, "log")) {
		if (HAP(r, "<tr><td><main role=\"main\">") ||
		    render_log(r, item[0], 50) ||
		    HAP(r, "</main></td></tr>"))
			return 1;
	} else if (!strcmp(r->mode, "commit")) {
		if (HAP(r, "<tr><td><main role=\"main\">") ||
		    render_commit(r, item[0]) ||
		    HAP(r, "</main></td></tr>"))
			return 1;
	} else if (!strcmp(r->mode, "tags")) {
		struct jg2_refsplit rs;

		reflist_split(item[0], &rs);
		if (HAP(r, "<tr><td><main role=\"main\"><table>") ||
		    render_tags(r, &rs, 999) ||
		    HAP(r, "</table></main></td></tr>"))
			return 1;
	} else if (!strcmp(r->mode, "branches")) {
		struct jg2_refsplit rs;

		reflist_split(item[0], &rs);
		if (HAP(r, "<tr><td><main role=\"main\"><table>") ||
		    render_branches(r, &rs, 999) ||
		    HAP(r, "</table></main></td></tr>"))
			return 1;
	} else if (!strcmp(r->mode, "summary")) {
		struct jg2_refsplit rs;

		reflist_split(item[0], &rs);
		if (HAP(r, "<tr><td><main role=\"main\"><table>") ||
		    render_branches(r, &rs, 10) ||
		    HAP(r, "<tr><td colspan=5>&nbsp;</td></tr>") ||
		    render_tags(r, &rs, 10) ||
		    HAP(r, "<tr><td colspan=5>&nbsp;</td></tr>") ||
		    render_log(r, nitems > 1 ? item[1] : item[0], 10) ||
		    HAP(r, "<tr><td colspan=5><a href=\"") ||
		    url_make(r, r->reponame, "log", r->rpath, NULL, NULL,
			   UQ_BRANCH) ||
		    HAP(r, "\">[...]</a></td></tr>"
			  "</table></main></td></tr>"))
			return 1;
	} else if (!strcmp(r->mode, "search")) {
		if (HAP(r, "<tr><td><div id=qindexing>") ||
		    render_search_files(r, item[0]) ||
		    render_tree(r, item, nitems) ||
		    HAP(r, "</td></tr></div></td></tr>"))
			return 1;
	} else {
		/* tree / blame / unknown-mode-as-tree */

		if (HAP(r, "<tr><td><main role=\"main\">") ||
		    render_tree(r, item, nitems) ||
		    HAP(r, "</main></td></tr>"))
			return 1;
	}

	return HAP(r, "</table>");
}

/* ----------------------------------------------------------------------
 * capture / render / drain plumbing
 */

int
jg2_ssr_capture_begin(struct jg2_ctx *ctx)
{
	if (ctx->cap_overflow) {
		/* discard sink: a small window at the end of cap_buf */

		ctx->buf = ctx->cap_buf + ctx->cap_size - JG2_SSR_BITBUCKET;
		ctx->p = ctx->buf;
		ctx->end = ctx->cap_buf + ctx->cap_size - 1;

		return 0;
	}

	if (!ctx->cap_buf) {
		ctx->cap_size = JG2_SSR_CAPTURE_SLICE * 2;
		ctx->cap_buf = malloc(ctx->cap_size);
		if (!ctx->cap_buf) {
			ctx->cap_size = 0;
			return 1;
		}
	} else {
		if (ctx->cap_size - ctx->cap_len < JG2_SSR_CAPTURE_SLICE +
			    JG2_RESERVE_SEAL) {
			size_t ns = ctx->cap_size * 2;

			if (ns > JG2_SSR_MAX_CAPTURE) {
				char *nb;

				ns = JG2_SSR_MAX_CAPTURE;
				if (ns <= ctx->cap_size) {
					ctx->cap_overflow = 1;
					return jg2_ssr_capture_begin(ctx);
				}

				nb = realloc(ctx->cap_buf, ns);
				if (!nb)
					return 1;

				ctx->cap_buf = nb;
				ctx->cap_size = ns;
			} else {
				char *nb = realloc(ctx->cap_buf, ns);

				if (!nb)
					return 1;

				ctx->cap_buf = nb;
				ctx->cap_size = ns;
			}
		}
	}

	ctx->buf = ctx->cap_buf;
	ctx->p = ctx->cap_buf + ctx->cap_len;
	ctx->end = ctx->cap_buf + ctx->cap_size - 1;

	return 0;
}

void
jg2_ssr_capture_end(struct jg2_ctx *ctx, char *wire_buf, char *wire_p,
		    char *wire_end)
{
	if (!ctx->cap_overflow && ctx->buf == ctx->cap_buf)
		ctx->cap_len = (size_t)(ctx->p - ctx->cap_buf);

	ctx->buf = wire_buf;
	ctx->p = wire_p;
	ctx->end = wire_end;
}

/* fill in the renderer state from the context + vhost config */

static void
srr_init(struct jg2_srr *r, struct jg2_ctx *ctx, struct jg2_hbuf *h)
{
	struct jg2_repodir *rd;
	const struct repo_entry_info *rei = NULL;

	memset(r, 0, sizeof(*r));

	r->ctx = ctx;
	r->h = h;
	r->vpath = ctx->vhost->cfg.virtual_base_urlpath;
	if (!r->vpath)
		r->vpath = "";
	r->reponame = ctx->sr.e[JG2_PE_NAME];
	r->mode = ctx->sr.e[JG2_PE_MODE];
	if (!r->mode || !is_known_mode(r->mode))
		r->mode = "tree";
	r->rpath = ctx->sr.e[JG2_PE_PATH];
	r->qbranch = ctx->sr.e[JG2_PE_BRANCH];
	r->qid = ctx->sr.e[JG2_PE_ID];
	r->qofs = ctx->sr.e[JG2_PE_OFFSET];
	r->qsearch = ctx->sr.e[JG2_PE_SEARCH];
	r->locale = ctx->locale;
	r->now = time(NULL);

#if LIBGIT2_HAS_BLAME
	r->caps |= 1;
#endif
#if defined(JG2_HAVE_ARCHIVE_H)
	r->caps |= 2;
#endif
	r->caps |= ctx->blog_mode << 2;

	if (ctx->flags & JG2_CTX_FLAG_BLAME_OVERLOADED)
		r->caps |= 8;

	if (ctx->vhost->cfg.avatar_url) {
		r->avatar_base = ctx->vhost->cfg.avatar_url;
		r->avatar_proxied = 1;
	} else {
		r->avatar_base = "//www.gravatar.com/avatar/";
		r->avatar_proxied = 0;
	}

	/* repo description / owner / url from the repodir info */

	rd = ctx->vhost->repodir;
	if (r->reponame && rd) {
		pthread_mutex_lock(&rd->lock);
		rei = __jg2_repodir_repo(rd, r->reponame);
		if (rei) {
			const char *s;

			s = jg2_rei_string(rei, REI_STRING_CONFIG_DESC);
			if (s && s[0] && s[0] != '+')
				r->repo.desc = s;
			s = jg2_rei_string(rei, REI_STRING_CONFIG_URL);
			if (s && s[0])
				r->repo.url = s;
		}
		pthread_mutex_unlock(&rd->lock);
	}
}

/*
 * The stats line, formatted from the translated template string.  This is
 * emitted in the uncached tail, so it is always fresh.
 */

static uint64_t
srr_timeval_us(struct timeval *t)
{
	return ((uint64_t)t->tv_sec * 1000000ull) + t->tv_usec;
}

static int
emit_stats_tail(struct jg2_srr *r)
{
	const char *tmpl = i18n(r,
		"Page fetched %{pf} ago, creation time: %{ct}ms "
		"(vhost etag hits: %{ve}%, cache hits: %{ch}%)");
	struct timeval t2;
	unsigned long ms;
	int pc = 0, pc1 = 0;
	const char *p = tmpl;

	gettimeofday(&t2, NULL);
	ms = (unsigned long)((srr_timeval_us(&t2) -
			((uint64_t)r->ctx->tv_gen.tv_sec * 1000000ull +
			 r->ctx->tv_gen.tv_usec)) / 1000);

	pthread_mutex_lock(&r->ctx->vhost->lock);
	if (r->ctx->vhost->cache_tries)
		pc = (int)((r->ctx->vhost->cache_hits * 100) /
			   r->ctx->vhost->cache_tries);
	if (r->ctx->vhost->etag_tries)
		pc1 = (int)((r->ctx->vhost->etag_hits * 100) /
			    r->ctx->vhost->etag_tries);
	pthread_mutex_unlock(&r->ctx->vhost->lock);

	if (HAP(r, "<div class=\"gitohashi-stats\" id=\"gitohashi-stats\">"
		  "<table><tr><td><img class='stats'></td><td>"))
		return 1;

	while (*p) {
		const char *ph = strstr(p, "%{");

		if (!ph) {
			if (ssr_esc(r, p))
				return 1;
			break;
		}

		if (ph != p && ssr_esc_len(r, p, (size_t)(ph - p)))
			return 1;

		if (!strncmp(ph, "%{pf}", 5)) {
			if (emit_age_plain(r, r->ctx->tv_gen.tv_sec))
				return 1;
			p = ph + 5;
		} else if (!strncmp(ph, "%{ct}", 5)) {
			if (jg2_hbuf_printf(r->h, "%lu", ms))
				return 1;
			p = ph + 5;
		} else if (!strncmp(ph, "%{ve}", 5)) {
			if (jg2_hbuf_printf(r->h, "%d", pc1))
				return 1;
			p = ph + 5;
		} else if (!strncmp(ph, "%{ch}", 5)) {
			if (jg2_hbuf_printf(r->h, "%d", pc))
				return 1;
			p = ph + 5;
		} else {
			if (HAP(r, "%{"))
				return 1;
			p = ph + 2;
		}
	}

	return HAP(r, "</td></tr></table></div>");
}

int
jg2_ssr_render_group(struct jg2_ctx *ctx)
{
	struct jg2_hbuf h;
	struct jg2_srr r;
	struct lwsac *ac = NULL;
	struct jg2_jn *items = NULL;
	int bad = 0, ret = 1;

	memset(&h, 0, sizeof(h));

	free(ctx->ssr_buf);
	ctx->ssr_buf = NULL;
	ctx->ssr_len = ctx->ssr_size = ctx->ssr_pos = 0;
	ctx->ssr_cache_len = 0;

	if (ctx->cap_overflow)
		bad = 1;
	else if (ctx->cap_len) {
		items = jg2_json_parse_seq(ctx->cap_buf, ctx->cap_len, &ac);
		if (!items) {
			lwsl_info("%s: internal JSON parse failed (%llu bytes)\n",
				  __func__,
				  (unsigned long long)ctx->cap_len);
			bad = 1;
		}
	}

	srr_init(&r, ctx, &h);

	/* the debug cache id rides along as a comment when present */

	if (items && items->child) {
		const char *cid = jg2_jn_str(jg2_jn_obj_get(items->child,
							    "cid"));

		if (cid && cid[0]) {
			jg2_hbuf_append(&h, "<!-- jg2 cid:", 13);
			ssr_esc_attr(&r, cid);
			jg2_hbuf_append(&h, " -->", 4);
		}
	}

	if (bad) {
		if (HAP(&r, "<table class='repobar'><tbody class='repobar'>"
			  "<tr><td><span class='error'><img class='warning'>"))
			goto bail;
		if (ctx->cap_overflow) {
			if (HAP(&r, "This content is too large to be "
				  "rendered as a web page, use the "
				  "plain view</span></td></tr></table>"))
				goto bail;
		} else {
			if (HAP(&r, "Internal rendering error"
				  "</span></td></tr></table>"))
				goto bail;
		}
	} else if (items && items->child) {
		if (render_page(&r, items->child))
			goto bail;
	} else {
		/* nothing captured (eg all items came from cache) */
	}

	/* everything up to here is per-content and cacheable */

	ctx->ssr_cache_len = h.len;

	/* the fresh tail: search index state + stats */

	if (!strcmp(r.mode, "search") && r.reponame) {
		uint32_t files = 0, done = 0;
		int idx = job_search_check_indexed(ctx, &files, &done);

		if (idx != 1) {
			if (HAP(&r, "<div class='jg2-indexing'>"))
				goto bail;
			if (!idx) {
				if (HAP(&r, "No search index for this "
					  "repository yet."))
					goto bail;
			} else {
				if (jg2_hbuf_printf(&h,
						"Building search index: "
						"%u / %u files", done, files))
					goto bail;
			}
			if (HAP(&r, "</div>"))
				goto bail;
		}
	}

	if (emit_stats_tail(&r))
		goto bail;

	ctx->ssr_buf = h.buf;
	ctx->ssr_len = h.len;
	ctx->ssr_size = h.size;
	ctx->ssr_pos = 0;

	/* the capture is consumed; reset for any later live group */

	ctx->cap_len = 0;
	ctx->cap_overflow = 0;

	ret = 0;

bail:
	if (ret)
		free(h.buf);

	lwsac_free(&ac);

	return ret;
}

int
jg2_ssr_drain(struct jg2_ctx *ctx)
{
	size_t left = (size_t)lws_ptr_diff(ctx->end, ctx->p);
	size_t rem = ctx->ssr_len - ctx->ssr_pos;
	size_t m = left < rem ? left : rem;

	if (m && ctx->ssr_buf) {
		memcpy(ctx->p, ctx->ssr_buf + ctx->ssr_pos, m);
		ctx->p += m;
		ctx->ssr_pos += m;
	}

	if (ctx->ssr_pos != ctx->ssr_len)
		return 0;	/* more to drain */

	/*
	 * Fully drained: persist the cacheable prefix.  We only hold a
	 * write-side fd when we generated the content live this transaction;
	 * a read-side fd from a cache hit was already closed by the spooler.
	 */

	if (ctx->fd_cache != -1 &&
	    ctx->job_cache_query != LWS_DISKCACHE_QUERY_EXISTS) {
		ssize_t n = ctx->ssr_cache_len ?
			write(ctx->fd_cache, ctx->ssr_buf, ctx->ssr_cache_len)
			: 0;

		if (n != (ssize_t)ctx->ssr_cache_len) {
			close(ctx->fd_cache);
			ctx->fd_cache = -1;
			unlink(ctx->cache);
		} else
			jg2_cache_write_complete(ctx);
	}

	free(ctx->ssr_buf);
	ctx->ssr_buf = NULL;
	ctx->ssr_len = ctx->ssr_size = ctx->ssr_pos = 0;

	return 1;		/* done */
}
