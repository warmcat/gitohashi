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

#define lp_to_rei(p, _n) list_ptr_container(p, struct repo_entry_info, _n)

static int
job_repos_start(struct jg2_ctx *ctx)
{
	ctx->rei = lp_to_rei(ctx->vhost->rei_head, next);

	meta_header(ctx);

	job_common_header(ctx);
	CTX_BUF_APPEND("\"repolist\":[");

	ctx->subsequent = 0;

	return 0;
}

static void
job_repos_destroy(struct jg2_ctx *ctx)
{
	ctx->job = NULL;
}

int
job_repos(struct jg2_ctx *ctx)
{
	char name[256];
	char *p;

	if (ctx->destroying) {
		job_repos_destroy(ctx);

		return 0;
	}

	if (!ctx->partway && job_repos_start(ctx)) {
		lwsl_err("%s: start failed (%s)\n", __func__, ctx->hex_oid);
		return -1;
	}

	if (!ctx->rei)
		goto empty;

	while (ctx->rei) {
		int size = ctx->rei->name_len + ctx->rei->acl_len +
			   ctx->rei->conf_len[0] + ctx->rei->conf_len[1] +
			   ctx->rei->conf_len[2];

		if (!JG2_HAS_SPACE(ctx, 100 + size))
			break;

		p = (char *)(ctx->rei + 1);


		pthread_mutex_lock(&ctx->vhost->lock); /* ======== vhost lock */

		if (__repo_check_acl(ctx->vhost, p, ctx->vhost->cfg.acl_user) &&
		    __repo_check_acl(ctx->vhost, p, ctx->acl_user)) {
			pthread_mutex_unlock(&ctx->vhost->lock); /*vhost lock */
			goto next;
		}

		pthread_mutex_unlock(&ctx->vhost->lock); /* ------ vhost lock */

		CTX_BUF_APPEND("%c\n{ \"reponame\": \"%s\"",
			       ctx->subsequent ? ',' : ' ',
			       ellipsis_purify(name, (char *)p, sizeof(name)));
		ctx->subsequent = 1;
		p += ctx->rei->name_len + ctx->rei->acl_len;

		if (ctx->rei->conf_len[0]) {
			CTX_BUF_APPEND(",\"desc\": \"%s\"",
				ellipsis_purify(name, (char *)p, sizeof(name)));

			p += ctx->rei->conf_len[0];
		}
		if (ctx->rei->conf_len[1]) {
			if (ctx->rei->conf_len[0])
				CTX_BUF_APPEND(", ");

			identity_json(p, ctx);
			p += ctx->rei->conf_len[1];
		}

		if (ctx->rei->conf_len[2]) {
			if (ctx->rei->conf_len[0] || ctx->rei->conf_len[1])
				CTX_BUF_APPEND(", ");

			CTX_BUF_APPEND(" \"url\": \"%s\"",
				ellipsis_purify(name, (char *)p, sizeof(name)));
		}

		CTX_BUF_APPEND("}");

next:
		ctx->rei = lp_to_rei(ctx->rei->next, next);
		if (ctx->rei)
			continue;
empty:
		meta_trailer(ctx, "]");
		job_repos_destroy(ctx);
	}

	return 0;
}
