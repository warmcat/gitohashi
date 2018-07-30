/*
 * libjg2 - ref list
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

static int
job_reflist_start(struct jg2_ctx *ctx)
{
	if (git_reference_iterator_new(&ctx->iter_ref, ctx->jrepo->repo) < 0)
		return -1;

	meta_header(ctx);

	job_common_header(ctx);
	CTX_BUF_APPEND("\"reflist\": [");

	return 0;
}

static void
job_reflist_destroy(struct jg2_ctx *ctx)
{
	if (ctx->iter_ref) {
		git_reference_iterator_free(ctx->iter_ref);
		ctx->iter_ref = NULL;
	}
	ctx->job = NULL;
}

int
job_reflist(struct jg2_ctx *ctx)
{
	if (ctx->destroying) {
		job_reflist_destroy(ctx);

		return 0;
	}

	if (!ctx->partway && job_reflist_start(ctx))
		return -1;

	while (JG2_HAS_SPACE(ctx, 768)) {
		char pure[128];
		git_reference *gref, *rref;
		const git_oid *oid;

		/*
		 * This will list 'commit' and 'tag' objects...
		 * 'commit' objects named refs/heads/xxx represent branch xxx
		 */

		if (git_reference_next(&gref, ctx->iter_ref) < 0) {
			meta_trailer(ctx, "\n]");
			job_reflist_destroy(ctx);
			break;
		}

		git_reference_resolve(&rref, gref);
		oid = git_reference_target(rref);
		if (oid) {
			jg2_json_purify(pure, git_reference_name(gref),
					sizeof(pure), NULL);
			CTX_BUF_APPEND("%c\n{ \"name\": \"%s\","
				       "\"summary\": ",
				       ctx->subsequent ? ',' : ' ', pure);
			ctx->subsequent = 1;
			generic_object_summary(oid, ctx);

			CTX_BUF_APPEND("}");
		}

		git_reference_free(rref);
		git_reference_free(gref);
	}

	return 0;
}
