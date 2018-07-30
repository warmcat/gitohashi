/*
 * libjsongit2 - commit
 *
 * Copyright (C) 2018 Andy Green <andy@warmcat.com>
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

#include "../private.h"

#include <stdio.h>
#include <string.h>

static int
common_print_cb(struct jg2_ctx *ctx, int origin,
		const char *content, size_t content_length)
{
	unsigned int u = content_length;
	char *p, do_pre = 0;

        switch (origin) {
        case GIT_DIFF_LINE_CONTEXT:
        case GIT_DIFF_LINE_ADDITION:
        case GIT_DIFF_LINE_DELETION:
        	u++;
        	do_pre = 1;
        	break;
        default:
        	break;
        }

	p = lac_use(&ctx->lac_head, u + 4, 0);
	if (!p)
		return -1;

	*p++ = u & 0xff;
	*p++ = (u >> 8) & 0xff;
	*p++ = (u >> 16) & 0xff;
	*p++ = (u >> 24) & 0xff;

	if (do_pre) {
		*p++ = origin;
		u--;
	}

	memcpy(p, content, u);

	return 0;
}

#if !LIBGIT2_HAS_DIFF
/*
 * print it into a lac, adding chunk buffers as needed, sized as needed.
 */
int patch_print_cb(
	const git_diff_delta *delta, /** delta that contains this data */
	const git_diff_range *range, /** range of lines containing this data */
	char line_origin,            /** git_diff_list_t value from above */
	const char *content,         /** diff data - not NUL terminated */
	size_t content_len,          /** number of bytes of diff data */
	void *payload)
{
	struct jg2_ctx *ctx = (struct jg2_ctx *)payload;

	return common_print_cb(ctx, line_origin, content, content_len);
}
#else
/*
 * callback interface is different at v0.24
 */
int patch_print_cb(
	const git_diff_delta *delta, /** delta that contains this data */
	const git_diff_hunk *hunk,   /**< hunk containing this data */
	const git_diff_line *line,   /**< line data */
	void *payload)
{
	struct jg2_ctx *ctx = (struct jg2_ctx *)payload;

	return common_print_cb(ctx, line->origin, line->content,
			       line->content_len);
}
#endif

static int
job_commit_start(struct jg2_ctx *ctx)
{
	git_tree *tp = NULL, *t = NULL;
	git_commit *parent = NULL;
#if !LIBGIT2_HAS_DIFF
	git_diff_list *d = NULL;
#else
	git_diff *d = NULL;
#endif
	git_generic_ptr u;
	git_oid oid;
	int e, ret = 1;

	if (!ctx->hex_oid[0]) {
		lwsl_err("%s: no oid\n", __func__);
		return 1;
	}

	if (ctx->hex_oid[0] == 'r') {
		e = git_reference_name_to_id(&oid, ctx->jrepo->repo,
						 ctx->hex_oid);
		if (e < 0) {
			lwsl_err("%s: unable to lookup ref '%s': %d\n",
				 __func__, ctx->hex_oid, e);
			return -1;
		}
	} else {
		e = git_oid_fromstr(&oid, ctx->hex_oid);
		if (e < 0) {
			lwsl_err("%s: git_oid_fromstr '%s': %d\n",
				 __func__, ctx->hex_oid, e);

			return -1;
		}
	}

	e = git_object_lookup(&u.obj, ctx->jrepo->repo, &oid, GIT_OBJ_ANY);
	if (e < 0) {
		lwsl_err("%s: git_object_lookup '%s': %d\n", __func__,
			 ctx->hex_oid, e);

		return -1;
	}

	if (git_object_type(u.obj) != GIT_OBJ_COMMIT) {
		lwsl_err("%s: not commit '%s'\n", __func__, ctx->hex_oid);
		git_object_free(u.obj);

		return -1;
	}

	ctx->raw_patch = !strcmp(ctx->sr.e[JG2_PE_MODE], "patch");

	ctx->u = u;
	if (!ctx->raw_patch) {
		meta_header(ctx);

	/* we issue three phases... 1) for html,the generic commit summary... */

		job_common_header(ctx);

		CTX_BUF_APPEND("\"commit\": {");

		commit_summary(ctx->u.commit, ctx);
	} else {

		/* ... for raw patch, synthesized header info */

		signature_text(git_commit_author(ctx->u.commit), ctx);
		CTX_BUF_APPEND("\n");
	}

	/* 2) if any, the commit message body (part after the first paragraph */

	ctx->body = git_commit_message(ctx->u.commit);
	if (!ctx->raw_patch && ctx->body)
		CTX_BUF_APPEND("},\n \"body\": \"");

	/* 3) if any, the diff itself */

	ctx->pos = 0;

	e = git_commit_parent(&parent, ctx->u.commit, 0);
	if (!e) {
		e = git_commit_tree(&tp, parent);
		if (e < 0) {
			lwsl_err("%s: git_commit_tree (parent) failed %d\n",
					__func__, e);
			goto bail2;
		}
	} else
		tp = NULL;

	e = git_commit_tree(&t, ctx->u.commit);
	if (e < 0) {
		lwsl_err("%s: git_commit_tree failed %d\n", __func__, e);
		goto bail1;
	}

	e = git_diff_tree_to_tree(&d, ctx->jrepo->repo, tp, t, NULL);
	if (e < 0) {
		lwsl_err("%s: git_diff_tree_to_tree failed %d\n", __func__, e);
		goto bail1;
	}

#if LIBGIT2_HAS_DIFF
	if (git_diff_print(d, GIT_DIFF_FORMAT_PATCH, patch_print_cb, ctx) < 0)
	{
		lwsl_err("%s: git_diff_print failed\n", __func__);
		goto bail;
	}
#else /* v0.19.0 */
	if (git_diff_print_patch(d, patch_print_cb, ctx) < 0)
	{
		lwsl_err("%s: git_diff_print_patch failed\n", __func__);
		goto bail;
	}
#endif
	ctx->lac = ctx->lac_head;
	ctx->pos = 0;
	ctx->size = 0;
	ctx->ofs = 0;

	if (!ctx->raw_patch && !ctx->body)
		CTX_BUF_APPEND("},\n \"diff\": \"");

	ret = 0;

bail:
#if LIBGIT2_HAS_DIFF
	git_diff_free(d);
#else
	git_diff_list_free(d);
#endif
bail1:
	if (tp)
		git_tree_free(tp);
bail2:
	if (t)
		git_tree_free(t);
	if (parent)
		git_commit_free(parent);

	if (ret)
		lwsl_err("%s: failed out\n", __func__);

	return ret;
}

static void
job_commit_destroy(struct jg2_ctx *ctx)
{
	if (ctx->u.commit) {
		git_commit_free(ctx->u.commit);
		ctx->u.commit = NULL;
	}

	lac_free(&ctx->lac_head);

	ctx->job = NULL;
}

int
job_commit(struct jg2_ctx *ctx)
{
	int m;

	if (ctx->destroying) {
		job_commit_destroy(ctx);

		return 0;
	}

	if (!ctx->partway && job_commit_start(ctx)) {
		lwsl_err("%s: start failed (%s)\n", __func__, ctx->hex_oid);
		return -1;
	}

	/* add body */

	while (ctx->body && JG2_HAS_SPACE(ctx, 768)) {
		size_t inlim_totlen = 0;

		if (!ctx->body)
			goto ended;

		/* control chars may bloat to 6x */
		while (inlim_totlen < 84 && ctx->body[inlim_totlen])
			inlim_totlen++;

		m = lws_ptr_diff(ctx->end, ctx->p) - 1;
		if (!ctx->raw_patch)
			ctx->p += jg2_json_purify(ctx->p, (char *)ctx->body, m,
					  &inlim_totlen);
		else {
			if (inlim_totlen > (size_t)m)
				inlim_totlen = m;
			memcpy(ctx->p, ctx->body, inlim_totlen);
			ctx->p += inlim_totlen;
		}
		ctx->body += inlim_totlen;

		if (!*ctx->body)
			ctx->body = NULL;

		if (!ctx->raw_patch && !ctx->body) {
			CTX_BUF_APPEND("\"\n", m);

			if (ctx->lac)
				CTX_BUF_APPEND(",\n \"diff\": \"");
			else
				CTX_BUF_APPEND("}\n");
		}

		if (ctx->raw_patch && !ctx->body)
			CTX_BUF_APPEND("\n\n");
	}

	while (!ctx->body && ctx->lac && JG2_HAS_SPACE(ctx, 512)) {
		size_t inlim_totlen = 84; /* control chars may bloat to 6x */
		char *p = (char *)(ctx->lac + 1) + ctx->ofs;

		if (!ctx->size) {
			ctx->size = (unsigned char)*p++;
			ctx->size |= ((unsigned char)*p++) << 8;
			ctx->size |= ((unsigned char)*p++) << 16;
			ctx->size |= ((unsigned char)*p++) << 24;

			ctx->pos = 0;
			ctx->ofs += 4;
		}

		if (inlim_totlen > ctx->size - ctx->pos)
			inlim_totlen = ctx->size - ctx->pos;

		if (ctx->raw_patch) {
			memcpy(ctx->p, p, inlim_totlen);
			ctx->p += inlim_totlen;
		} else
			ctx->p += jg2_json_purify(ctx->p, p,
					  lws_ptr_diff(ctx->end, ctx->p) - 4,
					  &inlim_totlen);
		ctx->pos += inlim_totlen;
		ctx->ofs += inlim_totlen;

		if (!inlim_totlen) {
			lwsl_err("%s: hit NUL\n", __func__);
			goto ended;
		}

		if (ctx->pos == ctx->size) {
			ctx->size = 0;
			ctx->ofs = lac_align(ctx->ofs);
			ctx->pos = 0;
		}

		if (ctx->ofs + sizeof(*ctx->lac) >= ctx->lac->ofs) {
			ctx->lac = ctx->lac->next;
			ctx->pos = 0;
			ctx->ofs = 0;
			if (!ctx->lac)
				goto ended;
		}
	}
	if (!ctx->body && !ctx->lac)
		goto ended;

	goto done;

ended:
	if (!ctx->raw_patch)
		meta_trailer(ctx, "\"");
	ctx->job = NULL;
	ctx->final = 1;
	ctx->body = NULL;
	job_commit_destroy(ctx);

done:
	return 0;
}
