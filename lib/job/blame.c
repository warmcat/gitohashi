/*
 * libjsongit2 - blame
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
 * Blame apis were only introduced in libgit2 v0.21.
 *
 * The blame apis show contiguous areas of lines as a hunk.  So if there was
 * an introductory commit 1 and two edits in the middle, 2 and 3, it shows
 * 1-2-1-3-1.
 *
 * Using a LAC we stitch this back into a list of commits, ordered by time,
 * each having an array of discontiguous line ranges.  So we issue 1-2-3, but
 * they're annotated with line range arrays.  This lets us highlight the
 * footprint of a particular contributing commit as a unit, even if it is
 * spread around the file.
 *
 * We also calculate a sorted list of contributors to the current file state,
 * in order of the number of lines for each.  To avoid reiterating the
 * identities again, since they are all by definition already mentioned in the
 * hunk list, we just give the a hunk index with the right identity.
 */

#include "../private.h"

#include <stdio.h>
#include <string.h>

#if LIBGIT2_HAS_BLAME
#define lp_to_bhi(p, _n) list_ptr_container(p, struct blame_hunk_info, _n)
#define lp_to_bli(p, _n) list_ptr_container(p, struct blame_line_range, _n)
#endif

static void
job_blame_destroy(struct jg2_ctx *ctx)
{
#if LIBGIT2_HAS_BLAME
	lac_free(&ctx->lac_head);
	ctx->sorted_head = NULL;

	if (ctx->blame) {
		git_blame_free(ctx->blame);
		ctx->blame = NULL;
	}
#endif
	ctx->job = NULL;
}

#if LIBGIT2_HAS_BLAME

static int
bhi_uniq_fsig_sort(list_ptr a, list_ptr b)
{
	struct blame_hunk_info *p1 = lp_to_bhi(a, next_sort_fsig),
			       *p2 = lp_to_bhi(b, next_sort_fsig);

	return p2->count_lines_rep_acc - p1->count_lines_rep_acc;
}

static int
bhi_date_sort(list_ptr a, list_ptr b)
{
	struct blame_hunk_info *p1 = lp_to_bhi(a, next),
			       *p2 = lp_to_bhi(b, next);

	return (int)(p1->final.when.time - p2->final.when.time);
}

static int
job_blame_start(struct jg2_ctx *ctx)
{
	if (!ctx->hex_oid[0]) {
		lwsl_err("no oid\n");
		return 1;
	}

	if (!ctx->sr.e[JG2_PE_PATH])
		return -1;

	git_blame_init_options(&ctx->blame_opts, GIT_BLAME_OPTIONS_VERSION);
	ctx->count = 0;

#if defined(JG2_HAVE_BLAME_MAILMAP)
	ctx->blame_opts.flags |= GIT_BLAME_USE_MAILMAP;
#endif

	if (ctx->hex_oid[0] == 'r') {
		int e = git_reference_name_to_id(&ctx->blame_opts.newest_commit,
						 ctx->jrepo->repo,
						 ctx->hex_oid);
		if (e < 0) {
			lwsl_err("%s: unable to lookup ref '%s': %d\n",
				 __func__, ctx->hex_oid, e);

			return -1;
		}
	} else
		if (git_oid_fromstr(&ctx->blame_opts.newest_commit,
				    ctx->hex_oid)) {
			lwsl_err("no oid from string\n");

			return -1;
		}

	ctx->blame_opts.min_line = 1;

	ctx->lac_head = NULL;
	ctx->sorted_head = NULL;
	ctx->pos = 0;

	ctx->blame_init_phase = 1;

	return 0;
}

static int
job_blame_lines(struct jg2_ctx *ctx)
{
	struct blame_hunk_info *bhi, *b;
	const git_blame_hunk *hunk;
	struct blame_line_range *r;
	int ord = 0;//, first = 1;
	list_ptr lp;

	// ctx->blame_opts.max_line = ctx->blame_opts.min_line + LINE_SET;
	ctx->blame_opts.min_line = 0;

	if (git_blame_file(&ctx->blame, ctx->jrepo->repo,
			   ctx->sr.e[JG2_PE_PATH], &ctx->blame_opts)) {
		lwsl_err("problem getting blame info\n");

		return -1;
	}

	// ctx->blame_opts.min_line += LINE_SET;

	/* process the blame chunks */

	do {
		hunk = git_blame_get_hunk_byindex(ctx->blame, ctx->count++);
	//	if (ctx->blame_opts.min_line - LINE_SET < 2203)
	//		first = 0;
		if (!hunk)
			continue;

		//first = 0;
		//ctx->count++;

		/* does "hunk->final_commit_id" already exist on our list? */

		bhi = NULL;
		lp = ctx->sorted_head;
		while (lp) {
			bhi = lp_to_bhi(lp, next);

			if (git_oid_equal(&bhi->hunk.final_commit_id,
					  &hunk->final_commit_id))
				break;

			list_ptr_advance(lp);
		}

		/* if it didn't already exist, add it */

		if (!lp) {
			git_commit *orig_commit, *final_commit;
			size_t len, s[7];
			char *p;

			if (git_commit_lookup(&orig_commit, ctx->jrepo->repo,
					      &hunk->orig_commit_id)) {
				lwsl_err("%s: Failed to find orig oid\n",
					 __func__);

				goto bail;
			}

			if (git_commit_lookup(&final_commit, ctx->jrepo->repo,
					      &hunk->final_commit_id)) {
				lwsl_err("%s: Failed to find final oid\n",
					 __func__);

				goto bail;
			}

			/*
			 * So the orig + final signatures, and hunk->orig_path,
			 * are given to us with temp pointers to the name and
			 * email info.
			 *
			 * We need to serialize them into composed git_signature
			 * members with the pointers fixed up to the copied
			 * names.
			 *
			 * In addition, we want to store the commit summaries
			 * for the original and final commit,
			 *
			 * [struct blame_hunk_info]
			 * [orig sig name]   NUL (<- bhi.orig.name)
			 * [orig sig email]  NUL (<- bhi.orig.email)
			 * [final sig name]  NUL (<- bhi.final.name)
			 * [final sig email] NUL (<- bhi.final.email)
			 * [orig cmmt log]   NUL (<- bhi.orig_summary)
			 * [final cmmt log]  NUL (<- bhi.final_summary)
			 * [orig path]       NUL (<- bhi.hunk.orig_path)
			 *
			 * Compute the true total length first...
			 */

			s[0] = strlen(hunk->orig_signature->name) + 1;
			s[1] = strlen(hunk->orig_signature->email) + 1;
			s[2] = strlen(hunk->final_signature->name) + 1;
			s[3] = strlen(hunk->final_signature->email) + 1;
			s[4] = strlen(git_commit_summary(orig_commit)) + 1;
			s[5] = strlen(git_commit_summary(final_commit)) + 1;
			s[6] = strlen(hunk->orig_path) + 1;

			len = sizeof(*bhi) +
			      s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6];

			bhi = lac_use(&ctx->lac_head, len, 0);
			if (!bhi) {
				lwsl_err("OOM\n");
				lac_free(&ctx->lac_head);

				return -1;
			}

			bhi->next_same_fsig = NULL;
			bhi->next_uniq_fsig = NULL;
			bhi->next_sort_fsig = NULL;
			bhi->fsig_rep = NULL;
			bhi->ordinal = 0;

			p = (char *)(bhi + 1);
			memcpy(&bhi->hunk, hunk, sizeof(*hunk));
			bhi->orig = *hunk->orig_signature;
			bhi->orig.name = p;
			memcpy(p, hunk->orig_signature->name, s[0]);
			p += s[0];
			bhi->orig.email = p;
			memcpy(p, hunk->orig_signature->email, s[1]);
			p += s[1];

			bhi->final = *hunk->final_signature;
			bhi->final.name = p;
			memcpy(p, hunk->final_signature->name, s[2]);
			p += s[2];
			bhi->final.email = p;
			memcpy(p, hunk->final_signature->email, s[3]);
			p += s[3];

			bhi->orig_summary = p;
			memcpy(p, git_commit_summary(orig_commit), s[4]);
			p += s[4];
			bhi->final_summary = p;
			memcpy(p, git_commit_summary(final_commit), s[5]);
			p += s[5];

			bhi->hunk.orig_path = p;
			memcpy(p, hunk->orig_path, s[6]);

			git_commit_free(orig_commit);
			git_commit_free(final_commit);

			bhi->line_range_head = NULL;
			/* tail is our head */
			bhi->line_range_tail = &bhi->line_range_head;

			bhi->count_line_ranges = 0;
			bhi->count_lines = 0;
			bhi->count_lines_rep_acc = 0;

			/* insert him into the bhi list using date order */

			list_ptr_insert(&ctx->sorted_head, &bhi->next,
					bhi_date_sort);

			/* Has his final sig already appeared? */

			lp = ctx->head_uniq_fsig;
			while (lp) {
				b = lp_to_bhi(lp, next_uniq_fsig);
				if (!strcmp(b->final.name, bhi->final.name) &&
				    !strcmp(b->final.email, bhi->final.email)) {
					/* add him to the list of same sig */
					bhi->next_same_fsig = b->next_same_fsig;
					b->next_same_fsig = bhi->next_same_fsig;
					bhi->fsig_rep = b;
					break;
				}
				lp = b->next_uniq_fsig;
			}

			if (!lp) {
				/*
				 * he has a unique final sig we didn't list yet,
				 * add him to the unique fsig list
				 */
				bhi->next_uniq_fsig = ctx->head_uniq_fsig;
				ctx->head_uniq_fsig = &bhi->next_uniq_fsig;
				bhi->fsig_rep = bhi;
			}
		}

		/*
		 * so bhi now points to this logical hunk in our sorted list...
		 * add this line range info to its list
		 */

		r = lac_use(&ctx->lac_head, sizeof(*r), 0);
		r->next = NULL;
		/* write our next's ads to last guy's next */
		*((void **)bhi->line_range_tail) = &r->next;
		/* last guy becomes us */
		bhi->line_range_tail = &r->next;
		bhi->count_line_ranges++;
		bhi->count_lines += hunk->lines_in_hunk;

		/*
		 * also accumulate all line counts in the bhi representing our
		 * unique final signature... so we can later sort the unique
		 * contributors by the number of their lines in the file easily
		 */
		bhi->fsig_rep->count_lines_rep_acc += hunk->lines_in_hunk;

		r->lines = hunk->lines_in_hunk;
		r->line_start_orig = hunk->orig_start_line_number;
		r->line_start_final = hunk->final_start_line_number;

	} while (hunk);

	if (ctx->blame) {
		git_blame_free(ctx->blame);
		ctx->blame = NULL;
	}

//	if (!first)
//		return 0;

	 /* he didn't produce anything, we are done with the initial stuff */
	ctx->blame_init_phase = 0;

	ctx->bhi = lp_to_bhi(ctx->sorted_head, next);
	ctx->bli = NULL;
	ctx->count = 0;

	/*
	 * Write each bhi's ordinal index, now the date sorting has been done.
	 * The contributor list wants to use it to show where the sig data is.
	 */

	lp = ctx->sorted_head;
	while (lp) {
		b = lp_to_bhi(lp, next);
		b->ordinal = ord++;
		lp = b->next;
	}

	/*
	 * create the sorted contributor list from the unique fsig reps and
	 * the line count accumulation on each we performed earlier.
	 *
	 * We're walking the "uniq" list, but creating in the "sort" list.
	 */

	ctx->head_sort_fsig = NULL;

	lp = ctx->head_uniq_fsig;
	while (lp) {
		b = lp_to_bhi(lp, next_uniq_fsig);
		list_ptr_insert(&ctx->head_sort_fsig, &b->next_sort_fsig,
				bhi_uniq_fsig_sort);
		lp = b->next_uniq_fsig;
	}

	/* initialize the iterator to walk the sort list */
	ctx->contrib = ctx->head_sort_fsig;

	meta_header(ctx);

	job_common_header(ctx);
	CTX_BUF_APPEND("\"oid\":");
	jg2_json_oid(&ctx->blame_opts.newest_commit, ctx);
	CTX_BUF_APPEND(",");

	CTX_BUF_APPEND("\"blame\": [");

	return 0;

bail:
	lwsl_err("%s: bailed\n", __func__);
	job_blame_destroy(ctx);

	return -1;
}
#endif

int
job_blame(struct jg2_ctx *ctx)
{
#if LIBGIT2_HAS_BLAME
	char pure[128];
#endif

	if (ctx->destroying) {
		job_blame_destroy(ctx);

		return 0;
	}

#if !LIBGIT2_HAS_BLAME
	ctx->final = 1;
#else

	if (!ctx->partway && job_blame_start(ctx)) {
		lwsl_err("%s: start failed (%s)\n", __func__, ctx->hex_oid);
		return -1;
	}

	if (ctx->blame_init_phase)
		return job_blame_lines(ctx);

	if (!ctx->bhi && !ctx->bli) {

		/* we are walking the "sort" list */

		while (ctx->contrib && JG2_HAS_SPACE(ctx, 72)) {
			struct blame_hunk_info *bhi =
				lp_to_bhi(ctx->contrib, next_sort_fsig);

			CTX_BUF_APPEND("%c\n{\"l\": %u,\"o\": %u}",
				       ctx->pos++ ? ',' : ' ',
				       bhi->count_lines_rep_acc, bhi->ordinal);

			ctx->contrib = bhi->next_sort_fsig;
		}

		if (ctx->contrib)
			return 0;

		/* we finished... */

		meta_trailer(ctx, "]");
		job_blame_destroy(ctx);

		return 0;
	}

	do {
		while (ctx->bli) {
			if (!JG2_HAS_SPACE(ctx, 72))
				break;

			CTX_BUF_APPEND("%c\n{\"l\": %u,\"o\": %u,\"f\": %u}",
				       ctx->pos++ ? ',' : ' ', ctx->bli->lines,
				       ctx->bli->line_start_orig,
				       ctx->bli->line_start_final);

			ctx->bli = lp_to_bli(ctx->bli->next, next);

			if (!ctx->bli)
				CTX_BUF_APPEND("]}");
		}

		if (ctx->bli) /* we ran out of buffer space */
			break;

		if (!ctx->bhi) {
			/* ...that was the last bhi we just finished.
			 * Move on to doing the sorted contributor list
			 */
			CTX_BUF_APPEND("],\n\"contrib\":[");
			ctx->pos = 0;

			break;
		}

		/* we need a new bhi */

		if (!JG2_HAS_SPACE(ctx, 768))
			break;

		/* we do have a new bhi to tell about */

		ctx->pos = 0;

		CTX_BUF_APPEND("%c\n{\"ord\":%d,\"orig_oid\":",
			       ctx->count++ ? ',' : ' ', ctx->bhi->ordinal);
		jg2_json_oid(&ctx->bhi->hunk.orig_commit_id, ctx);

		CTX_BUF_APPEND(",\n\"final_oid\":");
		jg2_json_oid(&ctx->bhi->hunk.final_commit_id, ctx);

		CTX_BUF_APPEND(",\n\"sig_orig\": ",
			(unsigned int)ctx->bhi->hunk.orig_start_line_number,
			(unsigned int)ctx->bhi->hunk.final_start_line_number);
		signature_json(&ctx->bhi->orig, ctx);

		CTX_BUF_APPEND(",\n\"log_orig\": \"%s\"",
				ellipsis_purify(pure, ctx->bhi->orig_summary,
						sizeof(pure)));

		CTX_BUF_APPEND(",\n\"sig_final\": ");
		signature_json(&ctx->bhi->final, ctx);

		CTX_BUF_APPEND(",\n\"log_final\": \"%s\"",
			       ellipsis_purify(pure, ctx->bhi->final_summary,
					       sizeof(pure)));

		/* only add orig_path if it differs from our starting path */
		if (ctx->sr.e[JG2_PE_PATH] &&
		    strcmp(ctx->sr.e[JG2_PE_PATH], ctx->bhi->hunk.orig_path)) {
			CTX_BUF_APPEND(",\n\"op\": \"%s\"",
				       ellipsis_purify(pure,
						       ctx->bhi->hunk.orig_path,
						       sizeof(pure)));
		}

		CTX_BUF_APPEND(",\n\"ranges\": [");

		ctx->bli = lp_to_bli(ctx->bhi->line_range_head, next);
		ctx->bhi = lp_to_bhi(ctx->bhi->next, next);

	} while (1);
#endif

	return 0;
}
