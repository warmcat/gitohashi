/*
 * libjg2 - plain
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
job_plain_start(struct jg2_ctx *ctx)
{
	ctx->meta_last_job = 1;
	return blob_from_commit(ctx);
}

static void
job_plain_destroy(struct jg2_ctx *ctx)
{
	if (ctx->u.blob) {
		git_blob_free(ctx->u.blob);
		ctx->u.blob = NULL;
	}
	ctx->job = NULL;
}


int
job_plain(struct jg2_ctx *ctx)
{
	size_t m;

	if (ctx->destroying) {
		job_plain_destroy(ctx);

		return 0;
	}

	if (!ctx->partway && job_plain_start(ctx)) {
		lwsl_err("%s: start failed (%s)\n", __func__, ctx->hex_oid);

		return -1;
	}

	if (ctx->body && JG2_HAS_SPACE(ctx, 1)) {
		/* we're sending a blob */

		m = lws_ptr_diff(ctx->end, ctx->p) - 1;

		if (m > ctx->size - ctx->pos)
			m = ctx->size - ctx->pos;

		memcpy(ctx->p, (char *)ctx->body + ctx->pos, m);
		ctx->pos += m;
		ctx->p += m;

		if (ctx->pos == ctx->size) {
			ctx->final = 1;
			job_plain_destroy(ctx);
		}
	}

	return 0;
}
