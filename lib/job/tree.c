/*
 * libjsongit2 - tree
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

#define lp_to_te(p, _n) lws_list_ptr_container(p, struct tree_entry_info, _n)

static int
tei_alpha_sort(lws_list_ptr a, lws_list_ptr b)
{
	struct tree_entry_info *p1 = lp_to_te(a, next),
			       *p2 = lp_to_te(b, next);

	/* directories go at the top */
	if ((p1->mode & 16384) != (p2->mode & 16384))
		return !!(p2->mode & 16384) - !!(p1->mode & 16384);

	return strcmp((const char *)(p1 + 1), (const char *)(p2 + 1));
}

/*
 * For efficiency, we dump results linearly in a linked-list of "chunk"
 * allocations, adding to it as needed.  It means we have much less allocation
 * that using one per-object.  See ./lib/lac/README.md
 *
 * Each entry is part of a sorted linked-list held inside the
 * struct tree_entry_info
 *
 * The objects consist of
 *
 *   [ struct tree_entry_info ] [ name string NUL terminated ]
 *
 */

static int
treewalk_cb(const char *root, const git_tree_entry *entry, void *payload)
{
	struct jg2_ctx *ctx = payload;
	struct tree_entry_info *tei;
	const char *name;
	git_blob *blob;
	git_otype type;
	size_t m;

	type = git_tree_entry_type(entry);
	name = git_tree_entry_name(entry);
	m = strlen(name) + 1;

	tei = lwsac_use(&ctx->lwsac_head, sizeof(*tei) + m, 0);
	if (!tei) {
		lwsl_err("OOM\n");

		return -1;
	}

	tei->oid = git_tree_entry_id(entry);
	tei->mode = git_tree_entry_filemode(entry);
	tei->namelen = m - 1;
	tei->type = type;
	tei->size = 0;

	if (type == GIT_OBJ_BLOB &&
	    !git_blob_lookup(&blob, ctx->jrepo->repo, tei->oid)) {
		tei->size = git_blob_rawsize(blob);
		git_blob_free(blob);
	}

	/* copy the name into place; lac is already advanced and aligned */

	memcpy(tei + 1, name, m);

	lws_list_ptr_insert(&ctx->sorted_head, &tei->next, tei_alpha_sort);

	return type == GIT_OBJ_TREE; /* don't go inside trees */
}

static void
job_tree_destroy(struct jg2_ctx *ctx)
{
	lwsl_err("%s\n", __func__);
	lwsac_free(&ctx->lwsac_head);
	ctx->sorted_head = NULL;

	if (ctx->u.tree) {
		lwsl_err("%s: free tree %p\n", __func__, ctx->u.tree);
		git_tree_free(ctx->u.tree);
		ctx->u.tree = NULL;
	}
	ctx->job = NULL;
}

static int
job_tree_start(struct jg2_ctx *ctx)
{
	git_tree_entry *te;
	git_generic_ptr u;
	const char *epath = ctx->sr.e[JG2_PE_PATH];
	char pure[256], entry_did_inline = ctx->did_inline;
	git_commit *c;
	git_oid oid;
	int e;

	if (!ctx->hex_oid[0]) {
		lwsl_err("%s: no oid\n", __func__);

		return 1;
	}

	if (!ctx->jrepo) {
		lwsl_err("%s: no jrepo\n", __func__);

		return 1;
	}

	ctx->count = 0;
	ctx->pos = 0;
	ctx->tei = NULL;

	if (ctx->hex_oid[0] == 'r') {
		e = git_reference_name_to_id(&oid, ctx->jrepo->repo,
					     ctx->hex_oid);
		if (e < 0) {
			lwsl_err("%s: unable to lookup ref '%s': %d\n",
				 __func__, ctx->hex_oid, e);

			return -1;
		}
	} else
		if (git_oid_fromstr(&oid, ctx->hex_oid)) {
			lwsl_err("no oid from string\n");

			return -1;
		}

	e = git_object_lookup(&u.obj, ctx->jrepo->repo, &oid, GIT_OBJ_ANY);
	if (e < 0) {
		lwsl_err("git_object_lookup failed\n");
		return -1;
	}

	if (git_object_type(u.obj) != GIT_OBJ_COMMIT) {
		lwsl_err("git object not a commit\n");
		goto bail;
	}

	/* convert the commit object to a tree object */

	c = u.commit;
	if (git_commit_tree(&u.tree, u.commit)) {
		lwsl_err("no tree from commit\n");
		goto bail;
	}

	lwsl_err("%s: touched tree %p +++++\n", __func__, u.tree);

	git_commit_free(c);

	lwsl_notice("%s\n", __func__);

	if (!ctx->did_inline && ctx->inline_filename[0]) {
		epath = ctx->inline_filename;
		lwsl_notice("using inline_filename %s\n", ctx->inline_filename);
		ctx->did_inline = 1;
	}

	if (epath && epath[0]) {
		if (git_tree_entry_bypath(&te, u.tree, epath)) {
			lwsl_err("%s: git_tree_entry_bypath %s failed\n",
				 __func__, epath);
			lws_snprintf(ctx->status, sizeof(ctx->status),
				     "Path '%s' doesn't exist in revision '%s'",
				     epath, ctx->hex_oid);

			goto bail;
		}

		lwsl_err("%s: free tree 1 %p\n", __func__, u.tree);
		git_tree_free(u.tree);
		u.tree = NULL;

		e = git_tree_entry_to_object(&u.obj, ctx->jrepo->repo, te);
		git_tree_entry_free(te);
		if (e) {
			lwsl_err("git_tree_entry_to_object failed\n");
			goto bail;
		}

		lwsl_err("%s: touch tree 1 %p\n", __func__, u.tree);
	}

	ctx->u = u;

	/*
	 * /tree/ mode urls are followed by a "path" element inside the tree.
	 *
	 * These can consist of either a dir path, which we want to use to
	 * restrict where we walk the tree, or a blob path.
	 *
	 * If it's a blob path, we don't walk the tree to show the dir listing,
	 * but show the blob as best we can.
	 */

	if (git_object_type(u.obj) == GIT_OBJ_BLOB) {
		const char *p = epath, *p1;

		ctx->body = git_blob_rawcontent(u.blob);
		ctx->size = git_blob_rawsize(u.blob);
		ctx->pos = 0;

		lwsl_notice("%s: blob\n", __func__);

		ctx->meta_last_job = 1;

		/* do some searchubf */

		if (ctx->sr.e[JG2_PE_SEARCH] && !ctx->did_sat &&
		    ctx->sr.e[JG2_PE_PATH]) {
			ctx->meta_last_job = 0;
			ctx->blame_after_tree =
				!strcmp(ctx->sr.e[JG2_PE_MODE], "blame");
		}

		/* countermand the FINAL if actually more to do ... */

		if (ctx->sr.e[JG2_PE_MODE] && !entry_did_inline &&
		    !strcmp(ctx->sr.e[JG2_PE_MODE], "blame")) {
			/* we want to send blame info after this */
			ctx->meta_last_job = 0;
			ctx->blame_after_tree =
				!strcmp(ctx->sr.e[JG2_PE_MODE], "blame");
		}

		meta_header(ctx);
		pure[0] = '\0';

		if (p) {
			p1 = strrchr(p + strlen(p) - 1, '/');
			if (p1)
				p = p1 + 1;
		}
		ellipsis_purify(pure, p, sizeof(pure));

		job_common_header(ctx);

		CTX_BUF_APPEND("\"oid\":");
		jg2_json_oid(&oid, ctx);
		CTX_BUF_APPEND(",\"blobname\": \"%s\", ", pure);

		if (!git_blob_is_binary(u.blob)) {
			CTX_BUF_APPEND("\"blob\": \"");
			return 0;
		}

		strncpy(pure, ctx->vhost->cfg.virtual_base_urlpath,
			sizeof(pure) - 1);
		pure[sizeof(pure) - 1] = '\0';

		if (strlen(pure) > 1)
			strcat(pure, "/");

		ellipsis_purify(pure + strlen(pure), ctx->sr.e[JG2_PE_NAME],
				sizeof(pure) - strlen(pure));
		CTX_BUF_APPEND(" \"bloblink\": \"%s/plain/", pure);
		ellipsis_purify(pure, epath, sizeof(pure) - 5);

		strcat(pure, "\" ");

		git_blob_free(u.blob);
		ctx->u.obj = NULL;
		ctx->body = NULL;

		meta_trailer(ctx, pure);
		job_tree_destroy(ctx);

		return 0;
	}

	/* so the first part is walk the tree level and collect the objects */

	e = git_tree_walk(ctx->u.tree, GIT_TREEWALK_PRE, treewalk_cb, ctx);
	if (e < 0) {
		const git_error *er = giterr_last();

		lwsl_err("Failed to collect the tree objects: %d\n", e);
		if (er)
			lwsl_err("err %d: %s\n", er->klass, er->message);

		goto bail;
	}

	ctx->tei = lp_to_te(ctx->sorted_head, next);

	meta_header(ctx);

	CTX_BUF_APPEND("{ \"schema\":\"libjg2-1\",\n \"oid\":");
	jg2_json_oid(&oid, ctx);
	CTX_BUF_APPEND(",\"tree\": [");

	lwsl_notice("%s: exit OK\n", __func__);

	return 0;

bail:
	if (u.obj) {
		git_object_free(u.obj);
		u.obj = NULL;
	}

	lwsac_free(&ctx->lwsac_head);

	ctx->failed_in_start = 1;
	job_tree_destroy(ctx);

	return -1;
}

struct inline_match {
	const char *name;
	unsigned char len;
};

/* in order of best preference... an entry earlier in this table is more
 * preferable and can replace an already-matched entry from later in the
 * table */

static struct inline_match inline_match[] = {
	{ "README.md", 9 },
	{ "README", 6 },
	{ ".mkd", 4 },
	{ ".md", 3 },
};

int
job_tree(struct jg2_ctx *ctx)
{
	struct tree_entry_info *head;
	char name[128];
	size_t m;

	if (ctx->destroying) {
		job_tree_destroy(ctx);

		return 0;
	}

	if (!ctx->partway && job_tree_start(ctx)) {
		lwsl_err("%s: start failed (%s)\n", __func__, ctx->hex_oid);
		return -1;
	}

	if (ctx->body) {
		/* we're sending a blob */

		size_t inlim_totlen = ctx->size - ctx->pos;

		m = lws_ptr_diff(ctx->end, ctx->p) - 1;
		if (m < JG2_RESERVE_SEAL + 24)
			return 0;

		m -= JG2_RESERVE_SEAL;

		if (inlim_totlen > m / 6)
			inlim_totlen = m / 6;

		if (!inlim_totlen)
			inlim_totlen = 1;

		ctx->p += jg2_json_purify(ctx->p, (char *)ctx->body + ctx->pos,
					  m, &inlim_totlen);
		ctx->pos += inlim_totlen;

		if (ctx->pos == ctx->size) {
			meta_trailer(ctx, "\"");
			job_tree_destroy(ctx);
		}
	}

	head = lp_to_te(ctx->sorted_head, next);

	while (ctx->tei) {
		const char *tei_name = (const char *)(ctx->tei + 1);
		size_t n;

		if (!JG2_HAS_SPACE(ctx, 250 + ctx->tei->namelen))
			break;

		CTX_BUF_APPEND("%c\n{ \"name\": \"%s\","
			       "\"mode\": \"%u\", \"size\":%llu}",
			       ctx->tei != head ? ',' : ' ',
			       ellipsis_purify(name, tei_name, sizeof(name)),
			       (unsigned int)ctx->tei->mode,
			       (unsigned long long)ctx->tei->size);

		/* is this file in the file listing an inline doc file? */

		for (n = 0; n < LWS_ARRAY_SIZE(inline_match); n++) {
			/*
			 * name has to be long enough to match; the end of the
			 * name must match the inline_match name; and we must
			 * not already have matched on the same or more
			 * preferable inline_match entry
			 */
			if (ctx->tei->namelen < inline_match[n].len ||
			    strcmp(tei_name + ctx->tei->namelen -
				   inline_match[n].len, inline_match[n].name) ||
			    (ctx->if_pref && ctx->if_pref <= n + 1))
				continue;

			if (ctx->sr.e[JG2_PE_PATH] && strlen(ctx->sr.e[JG2_PE_PATH]))
				if (ctx->sr.e[JG2_PE_PATH][strlen(ctx->sr.e[JG2_PE_PATH]) - 1] == '/')
					lws_snprintf(ctx->inline_filename,
					     sizeof(ctx->inline_filename),
					     "%s%s", ctx->sr.e[JG2_PE_PATH],
					     tei_name);
				else
					lws_snprintf(ctx->inline_filename,
					     sizeof(ctx->inline_filename),
					     "%s/%s", ctx->sr.e[JG2_PE_PATH],
					     tei_name);
			else
				lws_snprintf(ctx->inline_filename,
					     sizeof(ctx->inline_filename),
					     "%s", tei_name);

			/*
			 * note how preferable this match is, so we can
			 * supercede it if we find a better match
			 */
			ctx->if_pref = n + 1;

			/* we have a new job after this now... */
			ctx->meta_last_job = 0;
			break;
		}

		ctx->tei = lp_to_te(ctx->tei->next, next);

		if (ctx->tei)
			continue;

		meta_trailer(ctx, "]");
		job_tree_destroy(ctx);
	}

	return 0;
}
