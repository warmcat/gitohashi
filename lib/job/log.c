/*
 * libjg2 - log
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

static int
job_log_start(struct jg2_ctx *ctx)
{
	git_generic_ptr u;
	git_oid oid;
	int error;

	if (!ctx->hex_oid[0])
		return 1;

	if (ctx->hex_oid[0] == 'r') {
		error = git_reference_name_to_id(&oid, ctx->jrepo->repo,
						 ctx->hex_oid);
		if (error < 0) {
			lwsl_err("%s: unable to lookup ref '%s': %d\n",
				 __func__, ctx->hex_oid, error);
			return -1;
		}
	} else
		if (git_oid_fromstr(&oid, ctx->hex_oid))
			return -1;

	error = git_object_lookup(&u.obj, ctx->jrepo->repo, &oid, GIT_OBJ_ANY);
	if (error < 0)
		return -1;

	if (git_object_type(u.obj) != GIT_OBJ_COMMIT) {
		git_object_free(u.obj);

		return -1;
	}

	ctx->u = u;
	meta_header(ctx);

	job_common_header(ctx);

	CTX_BUF_APPEND("\"log\": [");
	ctx->subsequent = 0;

	return 0;
}

static void
job_log_destroy(struct jg2_ctx *ctx)
{
	if (ctx->u.commit) {
		git_commit_free(ctx->u.commit);
		ctx->u.commit = NULL;
	}
	ctx->job = NULL;
}

int
job_log(struct jg2_ctx *ctx)
{
	if (ctx->destroying) {
		job_log_destroy(ctx);

		return 0;
	}

	if (!ctx->partway && job_log_start(ctx)) {
		lwsl_err("%s: start failed (%s)\n", __func__, ctx->hex_oid);
		return -1;
	}

	while (JG2_HAS_SPACE(ctx, 768)) {
		git_commit *c;

		if (!ctx->u.obj || !ctx->count) {
			CTX_BUF_APPEND("\n]");

			if (ctx->u.obj) {
				git_commit_parent(&c, ctx->u.commit, 0);
				if (c) {
					CTX_BUF_APPEND(", \"next\": ");

					jg2_json_oid(git_commit_id(c), ctx);

					git_commit_free(c);
				}
			}

			meta_trailer(ctx, "");
			job_log_destroy(ctx);
			return 0;
		}

		CTX_BUF_APPEND("%c\n{ \"name\": ",
			       ctx->subsequent ? ',' : ' ');

		ctx->subsequent = 1;

		jg2_json_oid(git_commit_id(ctx->u.commit), ctx);

		CTX_BUF_APPEND(",\n"
				"\"summary\": {\n");

		commit_summary(ctx->u.commit, ctx);

		CTX_BUF_APPEND("}}");

		c = NULL;
		git_commit_parent(&c, ctx->u.commit, 0);
		git_commit_free(ctx->u.commit);
		ctx->u.commit = c;

		if (ctx->count)
			ctx->count--;
	}

	return 0;
}
