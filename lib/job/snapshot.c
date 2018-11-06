/*
 * libjg2 - snapshot
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
 *
 *
 * This statefully generates compressed archives on the fly according to the
 * requested URL in /snapshot/ mode.  The tree being archived is held open the
 * whole time and a maximum of one blob is held open at a time inbetween calls.
 * Everything related to the activity is held in the jg2_context and there's no
 * interaction between multiple ongoing snapshots each in its own context.
 *
 * The tree is not walked from libgit2 side but using a stateful directory
 * stack per context.
 *
 * To cover the impedence mismatch between the compressor buffer size and its
 * trigger to spill (on my libarchive version, it's spilled when the compressed
 * data reaches 64KiB or so, in lumps of 10KiB) and whatever the user buffer
 * size is, this code uses the LAC "linear_alloc_chunk" bufferlists as needed.
 * If you are using a 4KiB user buffer then, the peak buffer requirement in that
 * case from this code is 60KiB.
 */

#include "../private.h"

#include <stdio.h>
#include <string.h>

enum {
	COMP_TAR_GZ,
	COMP_TAR_BZ2,
	COMP_TAR_XZ,
	COMP_ZIP,
};

static ssize_t
a_write(struct archive *a, void *user, const void *p, size_t len)
{
	struct jg2_ctx *ctx = (struct jg2_ctx *)user;
	size_t avail = lws_ptr_diff(ctx->end, ctx->p), use = len;

	if (avail < use)
		use = avail;

	memcpy(ctx->p, p, use);
	ctx->p += use;

	if (use < len) {
		/*
		 * We can't really control how much the caller will issue.
		 *
		 * If it's more than we can pass on right now, collect it and
		 * anything else that comes before we can flush into the ctx
		 * LAC buffer
		 */

		char *chunk = lwsac_use(&ctx->lwsac_head, len - use, 0);

		if (!chunk)
			return -1;

		memcpy(chunk, (const char *)p + use, len - use);

		if (!ctx->lac) {
			ctx->lac = ctx->lwsac_head;
			ctx->lacpos = lwsac_sizeof();
		}

		lwsl_notice("%s: stashed in lac: %d, lacpos: %d\n", __func__,
			    (int)(len -use), (int)ctx->lacpos);
	}

	/* claim that we used everything, even if some went in a LAC */

	return len;
}

static int
a_open(struct archive *a, void *user)
{
	return 0;
}

static int
a_close(struct archive *a, void *user)
{
	return 0;
}


/*
 * the ref to snapshot, and the format to produce is found inside the "path":
 *
 * reponame-sortaref.format
 *
 * - reponame- is redundant
 * - sortaref is a branch name, tag name, or commit hash.
 * - format is .tar.bz2 etc
 *
 * Eg, myproject-v1.1.tar.bz2
 */

static int
job_snapshot_start(struct jg2_ctx *ctx)
{
	int e, l, n, comp = -1;
	const char *p, *p1;
	git_generic_ptr u;
	char pure[256];
	git_commit *c;
	git_oid oid;

	if (!ctx->hex_oid[0]) {
		lwsl_err("no oid\n");
		return 1;
	}

	ctx->count = 0;
	ctx->pos = 0;
	ctx->tei = NULL;

	if (!ctx->sr.e[JG2_PE_PATH]) {
		lwsl_err("%s: missing path\n", __func__);

		return -1;
	}

	p1 = strrchr(ctx->sr.e[JG2_PE_PATH], '-');
	if (!p1) {
		lwsl_err("%s: missing -rev...\n", __func__);

		return -1;
	}

	p1++;

	p = ctx->sr.e[JG2_PE_PATH];
	l = strlen(p);
	if (l < 8)
		return -1;

	if (!strcmp(p + l - 7, ".tar.gz")) {
		p += l - 7;
		comp = COMP_TAR_GZ;
	} else
		if (!strcmp(p + l - 8, ".tar.bz2")) {
			p += l - 8;
			comp = COMP_TAR_BZ2;
		} else
			if (!strcmp(p + l - 4, ".zip")) {
				p += l - 4;
				comp = COMP_ZIP;
			} else
				if (!strcmp(p + l - 7, ".tar.xz")) {
					p += l - 7;
					comp = COMP_TAR_XZ;
				} else {
					lwsl_err("%s: unknown archive type\n",
						 __func__);

					return -1;
				}

	p1 = strchr(ctx->sr.e[JG2_PE_PATH], '-');
	/*
	 * avoid false positive from coverity... we already checked above and
	 * know for certain there is a '-' in this string using strrchr...
	 */
	if (!p1)
		return -1;
	p1++;
	n = 0;
	while (p1 < p && n < (int)sizeof(ctx->hex_oid) - 1)
		ctx->hex_oid[n++] = *p1++;
	ctx->hex_oid[n] = '\0';

	/* priority 1: branch (refs/heads/) */

	lws_snprintf(pure, sizeof(pure), "refs/heads/%s", ctx->hex_oid);
	if (git_reference_name_to_id(&oid, ctx->jrepo->repo, pure)) {

		/* priority 2: tag (refs/tags/) */

		lws_snprintf(pure, sizeof(pure), "refs/tags/%s", ctx->hex_oid);
		if (git_reference_name_to_id(&oid, ctx->jrepo->repo, pure)) {

			/* priority 3: oid */

			if (git_oid_fromstr(&oid, ctx->hex_oid)) {
				lwsl_err("%s: can't interpret ref %s\n",
					 __func__, ctx->hex_oid);

				return -1;
			}
		}
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

	git_commit_free(c);

	ctx->a = archive_write_new();
	switch(comp) {
	case COMP_TAR_GZ:
		archive_write_add_filter_gzip(ctx->a);
		archive_write_set_format_ustar(ctx->a);
		break;
	case COMP_TAR_BZ2:
		archive_write_add_filter_bzip2(ctx->a);
		archive_write_set_format_ustar(ctx->a);
		break;
	case COMP_TAR_XZ:
		archive_write_add_filter_xz(ctx->a);
		archive_write_set_format_ustar(ctx->a);
		break;
	case COMP_ZIP:
		archive_write_set_format_zip(ctx->a);
		break;
	}

	e = archive_write_open(ctx->a, ctx, a_open, a_write, a_close);
	if (e) {
		lwsl_err("%s: archive_write_open said %d\n", __func__, e);

		goto bail;
	}

	n = lws_ptr_diff(p, ctx->sr.e[JG2_PE_PATH]);
	if (n > (int)sizeof(pure) - 2)
		n = (int)sizeof(pure) - 2;

	strncpy(pure, ctx->sr.e[JG2_PE_PATH], n);
	pure[n++] = '/';
	pure[n] = '\0';

	ctx->sp = 0;
	ctx->stack[ctx->sp].tree = u.tree;
	ctx->stack[ctx->sp].path = strdup(pure);
	if (!ctx->stack[ctx->sp].path)
		goto bail;
	ctx->stack[ctx->sp].index = 0;

	return 0;

bail:
	if (u.obj) {
		git_object_free(u.obj);
		u.obj = NULL;
	}

	archive_write_free(ctx->a);

	return -1;
}

static void
job_snapshot_destroy(struct jg2_ctx *ctx)
{
	int n = ctx->sp;

	while (n >= 0) {
		if (ctx->stack[n].tree) {
			if (ctx->stack[n].path)
				free(ctx->stack[n].path);
			if (ctx->stack[n].tree)
				git_tree_free(ctx->stack[n].tree);
			ctx->stack[n].tree = NULL;
		}
		n--;
	}

	if (ctx->u.obj) {
		git_object_free(ctx->u.obj);
		ctx->u.obj = NULL;
	}

	lwsac_free(&ctx->lwsac_head);

	ctx->job = NULL;
}

int
job_snapshot(struct jg2_ctx *ctx)
{
	struct archive_entry *ae;
	size_t avail, nc, use;
	char path[256];
	int n;

	if (ctx->destroying) {
		job_snapshot_destroy(ctx);

		return 0;
	}

	if (!ctx->partway && job_snapshot_start(ctx)) {
		lwsl_err("%s: start failed (%s)\n", __func__, ctx->hex_oid);
		return -1;
	}

	if (ctx->lac) {

		/*
		 * ah, we have some stashed in a side-buffer LAC we need to
		 * replay before we can issue any new archive data
		 */

		while (ctx->lac && ctx->p != ctx->end) {
			avail = lws_ptr_diff(ctx->end, ctx->p);
			use = nc = lwsac_get_tail_pos(ctx->lac) -  ctx->lacpos;

			if (use > avail)
				use = avail;

			lwsl_notice("%s: replay from lac use %d\n",
					__func__, (int)use);

			memcpy(ctx->p, ((char *)ctx->lac) + ctx->lacpos,
			       use);

			ctx->p += use;
			ctx->lacpos += use;

			if (ctx->lacpos == lwsac_get_tail_pos(ctx->lac)) {
				/* if any, move to next chunk... */
				ctx->lacpos = lwsac_sizeof();
				ctx->lac = lwsac_get_next(ctx->lac);

				/* if nothing left, free the LAC chain */
				if (!ctx->lac) {
					lwsl_notice("the lac chain is emptied\n");
					lwsac_free(&ctx->lwsac_head);
				} else
					lwsl_notice("moved to next lac\n");
			}
		}

		/*
		 * either ran out of onward buffer space,
		 * or replayed the whole LAC chain and can move on with new
		 * archive data...
		 */
	}

	if (!ctx->lac && ctx->waiting_replay_done && !ctx->archive_completion) {
		/*
		 * Close Step 2 is when we know we are waiting for no LAC left,
		 * there is no LAC left, and we want to close, logically close
		 * the archive and set a second flag  that next time we see
		 * no LAC left, we are really done.
		 */
		archive_write_close(ctx->a);

		ctx->archive_completion = 1;
	}

	while (!ctx->waiting_replay_done && !ctx->lac &&
	       JG2_HAS_SPACE(ctx, 1024)) {
		struct tree_iter_level *lev = &ctx->stack[ctx->sp];
		const git_tree_entry *te;

		/*
		 * are we in the middle of archiving an existing blob?
		 */

		if (ctx->body) {
			avail = lws_ptr_diff(ctx->end, ctx->p);
			use = nc = ctx->size - ctx->pos;

			if (use > avail)
				use = avail;

			archive_write_data(ctx->a, ctx->body + ctx->pos, use);

			ctx->pos += use;

			if (nc == use) {
				ctx->body = NULL;
				ctx->pos = 0;

				git_blob_free(ctx->u.blob);
				ctx->u.blob = NULL;
			}

			continue;
		}

		/*
		 * we need to archive the next thing in our tree walk then...
		 */

		te = git_tree_entry_byindex(lev->tree, lev->index++);
		if (!te) {

			/* this was the end of our current subtree... */

			free(lev->path);
			lev->path = NULL;
			git_tree_free(lev->tree);
			lev->tree = NULL;
			lev->index = 0;

			if (ctx->sp)
				/* let's go back up a level and continue... */
				ctx->sp--;
			else
				/*
				 * oh... we have finished the root tree...
				 *
				 * However we need to take care, the close
				 * action will usually flush pending write data
				 * that may require stashing in a LAC and
				 * replaying... so we have to stage the close,
				 * step 1 is set a flag we're waiting for all
				 * replay done...
				 */

				ctx->waiting_replay_done = 1;

			continue;
		}

		switch (git_tree_entry_type(te)) {

		case GIT_OBJ_TREE:

			if (ctx->sp == LWS_ARRAY_SIZE(ctx->stack) - 1) {
				lwsl_err("%s: too many dir levels %d\n",
					 __func__, ctx->sp + 1);

				goto error_out;
			}

			lws_snprintf(path, sizeof(path), "%s%s/", lev->path,
				     git_tree_entry_name(te));

			lev = &ctx->stack[ctx->sp + 1];
			if (git_tree_lookup(&lev->tree, ctx->jrepo->repo,
					    git_tree_entry_id(te))) {
				lwsl_err("%s: unable to get tree\n", __func__);

				goto error_out;
			}

			lev->path = strdup(path);
			if (!lev->path)
				goto error_out;

			lev->index = 0;

			/* officially go down to the next level */
			ctx->sp++;
			break;

		case GIT_OBJ_BLOB:

			if (git_blob_lookup(&ctx->u.blob, ctx->jrepo->repo,
					    git_tree_entry_id(te))) {
				lwsl_err("%s: unable to get blob\n", __func__);

				goto error_out;
			}

			ctx->body = git_blob_rawcontent(ctx->u.blob);
			ctx->pos = 0;
			ctx->size = git_blob_rawsize(ctx->u.blob);

			ae = archive_entry_new2(ctx->a);
			if (!ae) {
				lwsl_err("%s: unable to get tree\n", __func__);

				goto error_out;
			}

			lws_snprintf(path, sizeof(path), "%s%s", lev->path,
				     git_tree_entry_name(te));

			archive_entry_set_pathname(ae, path);
			archive_entry_set_perm(ae, git_tree_entry_filemode(te));
			archive_entry_set_size(ae, ctx->size);
			archive_entry_set_filetype(ae, AE_IFREG);

			n = archive_write_header(ctx->a, ae);
			archive_entry_free(ae);
			if (n) {
				lwsl_err("%s: problem writing header: %d\n",
					 __func__, n);

				goto error_out;
			}
			break;

		default:
			lwsl_err("%s: unexpected GIT_OBJ_ %d\n", __func__,
					git_tree_entry_type(te));

			goto error_out;
		}
	}

	if (ctx->archive_completion && !ctx->lac) {

		/*
		 * Close step 3 is seeing no LAC left pending again after
		 * step 2.
		 *
		 * It's really the end, with the archive logically closed, and
		 * anything buffered flushed into the caller buffer too.
		 */
		archive_write_free(ctx->a);
		ctx->a = NULL;

		ctx->final = 1;
		job_snapshot_destroy(ctx);
	}

	return 0;

error_out:
	lwsl_err("%s: failing out\n", __func__);

	archive_write_close(ctx->a);
	archive_write_free(ctx->a);

	ctx->final = 1;
	job_snapshot_destroy(ctx);

	return -1;
}
