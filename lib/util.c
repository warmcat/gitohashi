/*
 * libjsongit2 - wrapper for libgit2 with JSON IO
 *
 * Copyright (C) 2018 - 2020 Andy Green <andy@warmcat.com>
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
 */

#include "private.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <ctype.h>

#include <sys/time.h>

static const char *hex = "0123456789abcdef";

void
jg2_md5_upd_lenprefixed(struct jg2_vhost *vh, jg2_md5_context _ctx,
			const void *input, size_t ilen)
{
	uint32_t lp = (uint32_t)ilen;

	vh->cfg.md5_upd(_ctx, (unsigned char *)&lp, sizeof(lp));
	if (ilen)
		vh->cfg.md5_upd(_ctx, (const unsigned char *)input, ilen);
}

/*
 * repo_path should look like
 *
 *      reponame/mode/repopath[?h=branch][&id=oid]
 *
 * Notice this allocates.  So you must call jg2_repopath_destroy(sr).
 */

int
jg2_repopath_split(const char *urlpath, struct jg2_split_repopath *sr)
{
	char *p, *p1;
	int n;

	memset(sr, 0, sizeof(*sr));

	if (*urlpath == '/')
		urlpath++;

	sr->e[JG2_PE_NAME] = strdup(urlpath);
	if (!sr->e[JG2_PE_NAME])
		return 1;

	p = strchr(sr->e[JG2_PE_NAME], '/');
	if (p) {
		*p++ = '\0';

		sr->e[JG2_PE_MODE] = p;
		p = strchr(p, '/');
		if (p) {
			*p++ = '\0';
			if (*p)
				sr->e[JG2_PE_PATH] = p;
		} else
			p = (char *)sr->e[JG2_PE_MODE];
	} else
		p = (char *)sr->e[JG2_PE_NAME];

	/* sanitize url-provided repo name against .. attack */

	p1 = (char *)sr->e[JG2_PE_NAME];
	while ((p1 = strchr(p1, '.')))
		if (*(++p1) == '.') {
			lwsl_err("%s: illegal .. in repo path\n", __func__);

			goto bail;
		}

	/*
	 * Defense-in-depth: also reject ".." in the inside-repo path.  libgit2
	 * treats ".." as a literal entry name (so it is not currently
	 * exploitable for traversal via git_tree_entry_bypath/git_blame_file),
	 * but rejecting it here keeps the NAME and PATH handling consistent and
	 * removes any reliance on libgit2's interpretation staying this way.
	 */

	if (sr->e[JG2_PE_PATH]) {
		char *pp = (char *)sr->e[JG2_PE_PATH];

		while ((pp = strchr(pp, '.')))
			if (*(++pp) == '.') {
				lwsl_err("%s: illegal .. in repo path\n",
					 __func__);

				goto bail;
			}
	}

	/*
	 * Parse up to 4 query parameters.  Each iteration finds the next '?'
	 * (first param) or '&' separator, then the '=' inside "key=value".
	 * The key name spans [key_start, p) and we match it by exact length
	 * and content, rather than by the single character before '=' (the
	 * old code matched any key ending in the right letter, so "xq=..."
	 * was treated as the "q" search parameter).
	 */

	for (n = 0; n < 4; n++) {
		char *pp, *key_start, *eq;
		size_t klen;

		eq = strchr(p, !n ? '?' : '&');
		if (!eq)
			return 0;

		*eq++ = '\0';
		key_start = eq;

		eq = strchr(key_start, '=');
		if (!eq)
			return 0;

		/* key is [key_start, eq), value starts at eq + 1 */
		klen = (size_t)(eq - key_start);
		p = eq; /* p points at '=' for compatibility with below */

		if (klen == 1 && key_start[0] == 'h') {
			pp = strdup(p + 1);
			sr->e[JG2_PE_BRANCH] = (const char *)pp;
			while (*pp) {
				if (/* *pp != '_' && *pp != '-' && !isalnum(*pp) */ !*pp || *pp == '&') {
					*pp = '\0';
					break;
				}
				pp++;
			}
			// lwsl_err("%s: branch seen as %s\n", __func__, sr->e[JG2_PE_BRANCH]);

		}
		if (klen == 2 && !strncmp(key_start, "id", 2)) {/* id hex hash string*/
			pp = strdup(p + 1);
			sr->e[JG2_PE_ID] = (const char *)pp;

			while (*pp) {
				if (*pp < '0' || (*pp > '9' && *pp < 'A') || (*pp > 'Z' && *pp < 'a') || *pp > 'z') {
					*pp = '\0';
					break;
				}
				pp++;
			}
		}
		if (klen == 3 && !strncmp(key_start, "ofs", 3)) { /* ofs */
			sr->offset = atoi(p + 1);
		}
		if (klen == 1 && key_start[0] == 'q') {
			pp =  strdup(p + 1);
			sr->e[JG2_PE_SEARCH] = (const char *)pp;
			while (*pp) {
                               if (*pp != '_' && *pp != '-' && *pp != '.' && !isalnum(*pp)) {
                                       lwsl_err("%s: JG2_PE_SEARCH %s\n", __func__, sr->e[JG2_PE_SEARCH]);
					*pp = '\0';
					break;
				}
				pp++;
			}
		}
		/* advance p past the value for the next separator search */
		p = p + 1;
	}

	return 0;

bail:
	free((char *)sr->e[JG2_PE_NAME]);
	sr->e[JG2_PE_NAME] = NULL;

	return 1;
}

/*
 * This encapsulates master -> main fallback
 */

int
jg2_oid_lookup(git_repository *repo, git_oid *oid, const char *hex_oid)
{
	int error;

	if (!hex_oid[0])
		return 1;

	if (hex_oid[0] == 'r') {
		error = git_reference_name_to_id(oid, repo, hex_oid);
		if (error < 0) {
			if (!strcmp(hex_oid, "refs/heads/master")) {
				error = git_reference_name_to_id(oid, repo,
						"refs/heads/main");
			}
			if (error < 0) {
				lwsl_err("%s: unable to lookup ref '%s': %d\n",
					 __func__, hex_oid, error);
				return -1;
			}
		}
	} else
		if (git_oid_fromstr(oid, hex_oid))
			return -1;

	return 0;
}

const char *
jg2_ctx_get_path(const struct jg2_ctx *ctx, jg2_path_element type,
		 char *buf, size_t buflen)
{
	if (type == JG2_PE_VIRT_ID) {
		if (ctx->sr.e[JG2_PE_ID])
			return ctx->sr.e[JG2_PE_ID];
		if (ctx->sr.e[JG2_PE_BRANCH]) {
			lws_snprintf(buf, buflen, "refs/heads/%s",
				     ctx->sr.e[JG2_PE_BRANCH]);

			return buf;
		}

		if (ctx->jrepo && ctx->jrepo->repo) {
			git_reference *head;
			if (!git_repository_head(&head, ctx->jrepo->repo)) {
				const char *name = git_reference_name(head);
				if (name)
					lws_snprintf(buf, buflen, "%s", name);
				else
					lws_snprintf(buf, buflen, "refs/heads/master");
				git_reference_free(head);
				return buf;
			}
		}

		return "refs/heads/master";
	}

	if (type == JG2_PE_OFFSET)
		return (const char *)(void *)(intptr_t)ctx->sr.offset;

	return ctx->sr.e[type];
}

void
jg2_repopath_destroy(struct jg2_split_repopath *sr)
{
	if (sr->e[JG2_PE_BRANCH]) {
		free((char *)sr->e[JG2_PE_BRANCH]);
		sr->e[JG2_PE_BRANCH] = NULL;
	}
	if (sr->e[JG2_PE_ID]) {
		free((char *)sr->e[JG2_PE_ID]);
		sr->e[JG2_PE_ID] = NULL;
	}
	if (sr->e[JG2_PE_SEARCH]) {
		free((char *)sr->e[JG2_PE_SEARCH]);
		sr->e[JG2_PE_SEARCH] = NULL;
	}
	if (sr->e[JG2_PE_NAME]) {
		free((char *)sr->e[JG2_PE_NAME]);
		sr->e[JG2_PE_NAME] = NULL;
	}
}

/* returns amount written to escaped.
 *
 * if inlim_totlen is non-null, it restricts the amount of input that can be
 * used on input, and contains the amount of input used on output.
 *
 * The escaping itself is done by lws; we additionally ask for the HTML_SAFE
 * set, because gitohashi inlines JSON into the initial HTML page inside
 * <div id='initial-json'>, where a literal '<' or '&' would be parsed as
 * markup.  A JSON \u003c escape is invisible to JSON.parse() in the browser,
 * so this does not affect the js side, which must (and does) do its own HTML
 * escaping when it builds DOM from the parsed JSON.
 */

int
jg2_json_purify(char *escaped, const char *string, int len,
		size_t *inlim_totlen)
{
	int in_used = -1; /* unlimited */

	if (len < 1) {
		if (inlim_totlen)
			*inlim_totlen = 0;
		if (escaped)
			escaped[0] = '\0';

		return 0;
	}

	/*
	 * lws treats a positive *in_used as an input cap; 0 or negative means
	 * unlimited.  Preserve the old jg2 semantics that a cap of 0 means
	 * "process nothing" for callers that distinguish it (the diff
	 * streaming in job_commit).
	 */

	if (inlim_totlen) {
		if (!*inlim_totlen) {
			escaped[0] = '\0';

			return 0;
		}
		if (*inlim_totlen > 0x7fffffff)
			in_used = 0x7fffffff;
		else
			in_used = (int)*inlim_totlen;
	}

	lws_json_purify_flags(escaped, string, len, &in_used,
			      LWS_JSON_PURIFY_FLAG_HTML_SAFE);

	if (inlim_totlen)
		*inlim_totlen = (size_t)in_used;

	return (int)strlen(escaped);
}

void *
jg2_zalloc(size_t s)
{
	void *v;

	v = malloc(s);
	if (!v)
		return NULL;
	memset(v, 0, s);

	return v;
}

const char *
md5_to_hex_cstr(char *md5_hex_33, const unsigned char *md5)
{
	int n;

	if (!md5) {
		*md5_hex_33++ = '?';
		*md5_hex_33++ = '\0';
		return md5_hex_33 - 2;
	}
	for (n = 0; n < 16; n++) {
		*md5_hex_33++ = hex[((*md5) >> 4) & 0xf];
		*md5_hex_33++ = hex[*(md5++) & 0xf];
	}
	*md5_hex_33 = '\0';

	return md5_hex_33 - 32;
}

const char *
oid_to_hex_cstr(char *oid_hex, const git_oid *oid)
{
	if (!oid) {
		*oid_hex++ = 'x';
		*oid_hex++ = '\0';
		return oid_hex - 2;
	}
	git_oid_fmt(oid_hex, oid);
	oid_hex[GIT_OID_HEXSZ] = '\0';

	return oid_hex;
}

/*
 * Purify into out with a bound of max, adding "..." if the input had to be
 * truncated.  Truncation only happens inside jg2_json_purify(), which never
 * splits an escape sequence, so the result is always syntactically valid
 * JSON no matter what is in the input or how long it is.
 */

const char *
ellipsis_purify(char *out, const char *in, int max)
{
	size_t inlim = (size_t)max;
	int n;

	if (max < 8) {
		out[0] = '\0';

		return out;
	}

	/* leave room for the "..." if we end up truncating */

	n = jg2_json_purify(out, in, max - 3, &inlim);

	if (in && in[inlim])
		memcpy(out + n, "...", 4);

	return out;
}

void
time_json(const git_time *t, struct jg2_ctx *ctx)
{
	CTX_BUF_APPEND("{ \"time\": %llu, \"offset\": %d }",
		       (unsigned long long)t->time, t->offset);
}

void
name_email_json(const char *name, const char *email, struct jg2_ctx *ctx)
{
	char e[128], e1[64], md5_hex[33];

	if (!name)
		name = "unknown";

	if (!email)
		email = "unknown";

	md5_to_hex_cstr(md5_hex, email_md5(ctx->vhost, email));

	CTX_BUF_APPEND(" \"name\": \"%s\", \"email\": \"%s\", \"md5\": \"%s\" ",
		       ellipsis_purify(e, name, sizeof(e)),
		       ellipsis_purify(e1, email, sizeof(e1)), md5_hex);
}

/* try to parse out Name Name <name@name.com> into name and email */

void
identity_json(const char *name_email, struct jg2_ctx *ctx)
{
	char name[64], email[64], *p, *p1;
	size_t len;

	p1 = p = strchr(name_email, '<');
	if (p == name_email)
		goto try;

	if (!p) { /* there's no < */
		p = strchr(name_email, '@');
		if (!p)
			goto try;

		while (p > name_email && *p != ' ')
			p--;

		if (p == name_email)
			goto try;

		len = (int)(p - name_email);
		if (len >= sizeof(name))
			len = sizeof(name) - 1;
		strncpy(name, name_email, len);
		name[len] = '\0';

		name_email_json(name, p + 1, ctx);

		return;
	}

	if (p[-1] == ' ')
		p--;

	len = p - name_email;
	if (len > sizeof(name) - 1)
		len = sizeof(name) - 1;
	strncpy(name, name_email, len);
	name[len] = '\0';

	p1++;
	p = strchr(p1, '>');
	if (!p)
		goto try;

	len = p - p1;
	if (len > sizeof(name) - 1)
		len = sizeof(name) - 1;
	strncpy(email, p1, len);
	email[len] = '\0';

	name_email_json(name, email, ctx);

	return;

try:
	name_email_json(name_email, "", ctx);
}

void
signature_json(const git_signature *sig, struct jg2_ctx *ctx)
{
	CTX_BUF_APPEND("{ \"git_time\": ");
	time_json(&sig->when, ctx);

	CTX_BUF_APPEND(",");
	name_email_json(sig->name, sig->email, ctx);

	CTX_BUF_APPEND(" }");
}

/*
 * Bounded copy of in into out for text (non-JSON) contexts, like the
 * synthesized patch headers.  Control characters and DEL become spaces so a
 * crafted identity cannot inject fake headers into the patch text; other
 * bytes, including UTF-8, pass through so it stays readable.  Adds "..." if
 * the input had to be truncated.
 */

const char *
ellipsis_text(char *out, const char *in, int max)
{
	const char *p = in;
	char *q = out;
	int budget = max - 4; /* leave room for "..." and NUL */

	if (!p || budget < 1) {
		out[0] = '\0';

		return out;
	}

	while (budget-- > 0 && *p) {
		unsigned char c = (unsigned char)*p++;

		if (c < 0x20 || c == 0x7f)
			c = ' ';

		*q++ = (char)c;
	}

	if (*p) {
		*q++ = '.';
		*q++ = '.';
		*q++ = '.';
	}
	*q = '\0';

	return out;
}

void
signature_text(const git_signature *sig, struct jg2_ctx *ctx)
{
	struct tm tm, *tmr;
	char dt[96], n[128], e[128];

	tmr = localtime_r((const time_t *)&sig->when.time, &tm);

	/*
	 * sig->name and sig->email are attacker-controllable (commit author
	 * fields).  This is text output, so we keep it readable but stop
	 * control characters from injecting new patch headers.
	 */

	CTX_BUF_APPEND("Author: %s <%s>\n",
		       ellipsis_text(n, sig->name ? sig->name : "unknown",
				       sizeof(n)),
		       ellipsis_text(e, sig->email ? sig->email : "unknown",
				       sizeof(e)));

	if (tmr) {
		/* like Date:   Mon Aug 13 16:49:58 2018 +0800 */
		strftime(dt, sizeof(dt), "%a %h %d %H:%M:%S %Y %z", &tm);
		CTX_BUF_APPEND("Date: %s\n", dt);
	}
}

static const char * otype_table[] = {
	"any",
	"bad",
	"ext1",
	"commit",
	"tree",
	"blob",
	"tag",
	"ext2",
	"ofs-delta",
	"ref-delta"
};

const char *
otype_name(git_otype type)
{
	size_t ntype = type + 2;

	if (ntype >= LWS_ARRAY_SIZE(otype_table))
		return "unknown";

	return otype_table[ntype];
}

int
jg2_oid_to_ref_names(const git_oid *oid, struct jg2_ctx *ctx,
		     struct jg2_ref **result, int max)
{
	struct jg2_ref *ref;
	int n = 0;

	pthread_mutex_lock(&ctx->jrepo->lock);

	ref = ctx->jrepo->ref_list_hash[jg2_oidbin(oid)];

	while (ref) {
		if (!git_oid_cmp(&ref->oid, oid)) {
			*result++ = ref;
			if (++n == max)
				break;
		}
		ref = ref->hash_next;
	}

	pthread_mutex_unlock(&ctx->jrepo->lock);

	return n;
}

int
jg2_json_oid(const git_oid *oid, struct jg2_ctx *ctx)
{
	char oid_hex[GIT_OID_HEXSZ + 1], pure[32];
	struct jg2_ref *aliases[8];
	int n, m = 0;

	n = jg2_oid_to_ref_names(oid, ctx, aliases, LWS_ARRAY_SIZE(aliases));

	CTX_BUF_APPEND("{ \"oid\": \"%s\", "
		       "\"alias\": [", oid_to_hex_cstr(oid_hex, oid));

	while (m < n) {
		CTX_BUF_APPEND("%c\"%s\"", !m ? ' ' : ',', ellipsis_purify(pure,
			       aliases[m]->ref_name, sizeof(pure)));
		m++;
	}

	CTX_BUF_APPEND("]}");

	return 0;
}

int
commit_summary(git_commit *commit, struct jg2_ctx *ctx)
{
	char summary[100];

	CTX_BUF_APPEND("\"type\":\"commit\",\n \"time\": %llu,\n"
			"\"time_ofs\": %llu,\n \"oid_tree\": ",
			(unsigned long long)git_commit_time(commit),
			(unsigned long long)git_commit_time_offset(commit));

	jg2_json_oid(git_commit_tree_id(commit), ctx);

	CTX_BUF_APPEND(",\n\"oid\":");

	jg2_json_oid(git_commit_id(commit), ctx);

	CTX_BUF_APPEND(",\n \"msg\": \"%s\",\n \"sig_commit\": ",
		ellipsis_purify(summary,
#if LIBGIT2_HAS_DIFF
				git_commit_summary(commit),
#else
				git_commit_message(commit),
#endif
				sizeof(summary)));

	signature_json(git_commit_committer(commit), ctx);

	CTX_BUF_APPEND(",\n\"sig_author\": ");

	signature_json(git_commit_author(commit), ctx);

	return 0;
}

/* these summaries have restricted sizes below 512 bytes */

int
generic_object_summary(const git_oid *oid, struct jg2_ctx *ctx)
{
	char summary[100];
	git_generic_ptr u;
	git_otype type;
	int e;

	if (!oid) {
		CTX_BUF_APPEND("{}");

		return 0;
	}

	e = git_object_lookup(&u.obj, ctx->jrepo->repo, oid, GIT_OBJ_ANY);
	if (e < 0) {
		CTX_BUF_APPEND("{}");

		return 0;
	}

	type = git_object_type(u.obj);
	if (type < GIT_OBJ_COMMIT || type > GIT_OBJ_TAG) {
		CTX_BUF_APPEND("{}");

		return 0;
	}

	CTX_BUF_APPEND("{ ");

	switch (type) {
	case GIT_OBJ_COMMIT:
		commit_summary(u.commit, ctx);
		break;

	case GIT_OBJ_TREE:
		CTX_BUF_APPEND("\"type\":\"tree\"\n");
		break;
	case GIT_OBJ_BLOB:
		CTX_BUF_APPEND("\"type\":\"blob\",\n \"size\":\"%llu\"",
			(unsigned long long)git_blob_rawsize(u.blob));
		break;
	case GIT_OBJ_TAG:
		CTX_BUF_APPEND("\"type\":\"tag\",\n \"oid_tag\": ");

		jg2_json_oid(git_tag_target_id(u.tag), ctx);

		CTX_BUF_APPEND(",\n \"type_tag\": \"%s\",\n"
			"\"msg_tag\": \"%s\",\n \"sig_tagger\": ",
			otype_name(git_tag_target_type(u.tag)),
			ellipsis_purify(summary, git_tag_message(u.tag),
					sizeof(summary)));

		signature_json(git_tag_tagger(u.tag), ctx);
		break;

	default:
		break;
	}

	CTX_BUF_APPEND("}");
	git_object_free(u.obj);

	return 0;
}

int
blob_from_commit(struct jg2_ctx *ctx)
{
	git_tree_entry *te;
	git_generic_ptr u;
	char branch[128];
	git_commit *c;
	git_oid oid;
	int e;

	if (!ctx->hex_oid[0])
		return 1;

	ctx->count = 0;

	if (strlen(ctx->hex_oid) != 40 &&
	    strncmp(ctx->hex_oid, "refs/heads/", 11)) {
		strcpy(branch, "refs/heads/");
		strncpy(branch + 11, ctx->hex_oid, sizeof(branch) - 12);
	} else
		strncpy(branch, ctx->hex_oid, sizeof(branch) - 1);
	branch[sizeof(branch) - 1] = '\0';

	if (branch[0] == 'r') {
		e = git_reference_name_to_id(&oid, ctx->jrepo->repo, branch);
		if (e < 0) {
			if (!strcmp(branch, "refs/heads/master"))
				e = git_reference_name_to_id(&oid, ctx->jrepo->repo, "refs/heads/main");
			if (e < 0) {
				lwsl_err("%s: unable to lookup ref '%s'('%s'): %d\n",
						__func__, ctx->hex_oid, branch, e);

				return -1;
			}

			lwsl_debug("%s: fell back to main to get blob\n", __func__);
		}
	} else
		if (git_oid_fromstr(&oid, ctx->hex_oid)) {
			lwsl_err("oid_fromstr failed\n");

			return -1;
		}

	e = git_object_lookup(&u.obj, ctx->jrepo->repo, &oid, GIT_OBJ_ANY);
	if (e < 0) {
		lwsl_err("object lookup failed\n");

		return -1;
	}

	if (git_object_type(u.obj) != GIT_OBJ_COMMIT) {
		lwsl_err("object not a commit\n");

		goto bail;
	}

	/* convert the commit object to a tree object */

	c = u.commit;
	if (git_commit_tree(&u.tree, u.commit)) {
		lwsl_err("git_commit_tree failed\n");

		goto bail;
	}

	git_commit_free(c);

	// lwsl_notice("%s: PE_PATH %s\n", __func__, ctx->sr.e[JG2_PE_PATH]);

	if (ctx->sr.e[JG2_PE_PATH]) {

		if (git_tree_entry_bypath(&te, u.tree,
					  ctx->sr.e[JG2_PE_PATH])) {
			lwsl_err("%s: git_tree_entry_bypath %s failed\n",
				 __func__, ctx->sr.e[JG2_PE_PATH]);

			goto bail;
		}

		git_tree_free(u.tree);
		u.obj = NULL;

		e = git_tree_entry_to_object(&u.obj, ctx->jrepo->repo, te);
		git_tree_entry_free(te);
		if (e) {
			lwsl_err("tree_entry_to_object failed\n");

			goto bail;
		}
	}

	ctx->u = u;

	/*
	 * /plain/ mode urls are followed by a "path" element inside the tree.
	 *
	 * These can consist of either a dir path, which we want to use to
	 * restrict where we walk the tree, or a blob path.
	 */

	if (git_object_type(u.obj) != GIT_OBJ_BLOB) {
		lwsl_err("object is not a blob (%d)\n", git_object_type(u.obj));

		goto bail;
	}

	ctx->body = git_blob_rawcontent(u.blob);
	ctx->size = git_blob_rawsize(u.blob);
	ctx->pos = 0;

	return 0;

bail:
	if (u.obj) {
		git_object_free(u.obj);
		u.obj = NULL;
	}

	return -1;
}
