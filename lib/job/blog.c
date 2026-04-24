/*
 * libjsongit2 - blog
 *
 * Copyright (C) 2018-2026 Andy Green <andy@warmcat.com>
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
 * If the vhost is in blog mode, then the urplaths have no repository part,
 * it's faked to be as set by the vhost init args, and no mode part.
 *
 *  /vpath[/path]
 *
 * If there is a path part, the mode is faked to be "tree" to use the existing
 * inline blob capability and provide by-year and by-month "directory listings"
 * reusing the tree dir listing as well.
 *
 * If there's no path part, we end up here to provide a paginated overview
 * listing in time order.
 */

#include "../private.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define lp_to_te(p, _n) lws_list_ptr_container(p, struct tree_entry_info, _n)

static int
tei_rev_alpha_sort(lws_list_ptr a, lws_list_ptr b)
{
	struct tree_entry_info *p1 = lp_to_te(a, next),
			       *p2 = lp_to_te(b, next);

	/* compare reverse alphabetically so newest date comes first */
	return strcmp((const char *)(p2 + 1), (const char *)(p1 + 1));
}

static int
is_all_digits(const char *s, int len)
{
	int i;
	for (i = 0; i < len; i++) {
		if (!isdigit(s[i]))
			return 0;
	}
	return 1;
}

static int
treewalk_cb_blog(const char *root, const git_tree_entry *entry, void *payload)
{
	struct jg2_ctx *ctx = payload;
	struct tree_entry_info *tei;
	const char *name;
	git_otype type;
	int rlen, nlen;

	type = git_tree_entry_type(entry);
	name = git_tree_entry_name(entry);
	rlen = strlen(root);
	nlen = strlen(name);

	if (type == GIT_OBJ_TREE) {
		if (rlen == 0) { /* year */
			if (nlen == 4 && is_all_digits(name, 4)) {
				lwsl_notice("%s: descending year %s\n", __func__, name);
				return 0;
			}
		} else if (rlen == 5) { /* root = "YYYY/" */
			if (nlen == 2 && is_all_digits(name, 2)) {
				lwsl_notice("%s: descending month %s%s\n", __func__, root, name);
				return 0;
			}
		} else if (rlen == 8) { /* root = "YYYY/MM/" */
			if (nlen == 2 && is_all_digits(name, 2)) {
				lwsl_notice("%s: descending day %s%s\n", __func__, root, name);
				return 0;
			}
		}
		lwsl_notice("%s: skipping tree %s%s\n", __func__, root, name);
		return 1; /* Skip other trees */
	}

	if (type == GIT_OBJ_BLOB) {
		if ((rlen == 11 || rlen == 8 || rlen == 5) && nlen > 3) {
			/* Inside YYYY/MM/DD/, YYYY/MM/ or YYYY/, check for .md or .mkd */
			if (!strcmp(name + nlen - 3, ".md") || (nlen > 4 && !strcmp(name + nlen - 4, ".mkd"))) {
				size_t m = rlen + nlen + 1;
				tei = lwsac_use(&ctx->lwsac_head, sizeof(*tei) + m, 0);
				if (!tei) {
					lwsl_err("%s: lwsac_use failed for %s\n", __func__, name);
					return -1;
				}
				
				tei->mode = git_tree_entry_filemode(entry);
				tei->namelen = m - 1; /* name + root length */
				tei->type = GIT_OBJ_BLOB;
				tei->size = 0;

				git_oid *oid_copy = lwsac_use(&ctx->lwsac_head, sizeof(*oid_copy), 0);
				if (!oid_copy) {
					lwsl_err("%s: lwsac_use failed for oid\n", __func__);
					return -1;
				}
				*oid_copy = *git_tree_entry_id(entry);
				tei->oid = oid_copy;

				char *p = (char *)(tei + 1);
				if (rlen)
					memcpy(p, root, rlen);
				memcpy(p + rlen, name, nlen + 1);

				lwsl_notice("%s: added blob %s\n", __func__, p);

				lws_list_ptr_insert(&ctx->sorted_head, &tei->next, tei_rev_alpha_sort);
			}
		}
	}

	return 0;
}

static void
job_blog_destroy(struct jg2_ctx *ctx)
{
	if (ctx->iter_ref) {
		git_reference_iterator_free(ctx->iter_ref);
		ctx->iter_ref = NULL;
	}

	lwsac_free(&ctx->lwsac_head);
	ctx->sorted_head = NULL;

	if (ctx->u.tree) {
		git_tree_free(ctx->u.tree);
		ctx->u.tree = NULL;
	}
	ctx->job = NULL;
}

static int
job_blog_start(struct jg2_ctx *ctx)
{
	git_generic_ptr u;
	git_commit *c;
	git_oid oid;
	int e;

	if (!ctx->hex_oid[0]) return 1;
	if (!ctx->jrepo) return 1;

	ctx->count = 0;
	ctx->pos = 0;
	ctx->tei = NULL;

	e = jg2_oid_lookup(ctx->jrepo->repo, &oid, ctx->hex_oid);
	if (e) return e;

	e = git_object_lookup(&u.obj, ctx->jrepo->repo, &oid, GIT_OBJ_ANY);
	if (e < 0) return -1;

	if (git_object_type(u.obj) != GIT_OBJ_COMMIT) {
		git_object_free(u.obj);
		return -1;
	}

	c = u.commit;
	if (git_commit_tree(&u.tree, u.commit)) {
		git_commit_free(c);
		return -1;
	}
	git_commit_free(c);

	ctx->u = u;

	e = git_tree_walk(ctx->u.tree, GIT_TREEWALK_PRE, treewalk_cb_blog, ctx);
	if (e < 0) {
		lwsl_err("%s: git_tree_walk failed %d\n", __func__, e);
		job_blog_destroy(ctx);
		return -1;
	}

	ctx->tei = lp_to_te(ctx->sorted_head, next);
	lwsl_notice("%s: walk complete, ctx->tei is %p\n", __func__, ctx->tei);

	meta_header(ctx);
	job_common_header(ctx);
	CTX_BUF_APPEND("\"reflist\": [");

	return 0;
}

int
job_blog(struct jg2_ctx *ctx)
{
	git_blob *blob;
	char title[256];
	char summary[1024];
	char image_route[256];
	char pure_title[512];
	char pure_summary[2048];
	char pure_image[512];

	if (ctx->destroying) {
		job_blog_destroy(ctx);
		return 0;
	}

	if (!ctx->partway) {
		if (job_blog_start(ctx)) {
			lwsl_err("%s: job_blog_start failed\n", __func__);
			meta_trailer(ctx, "\n]");
			return -1;
		}
		ctx->partway = 1;
	}

	lwsl_notice("%s: entering loop, ctx->tei=%p\n", __func__, ctx->tei);

	while (ctx->tei) {
		if (!JG2_HAS_SPACE(ctx, 2500)) {
			lwsl_notice("%s: JG2_HAS_SPACE failed (space needed 2500)\n", __func__);
			break;
		}

		const char *path = (const char *)(ctx->tei + 1);
		lwsl_notice("%s: processing %s\n", __func__, path);
		
		title[0] = '\0';
		summary[0] = '\0';
		image_route[0] = '\0';

		if (git_blob_lookup(&blob, ctx->jrepo->repo, ctx->tei->oid)) {
			lwsl_err("%s: git_blob_lookup failed for %s\n", __func__, path);
			ctx->tei = lp_to_te(ctx->tei->next, next);
			continue;
		}

		lwsl_notice("%s: blob opened for %s\n", __func__, path);
		const char *content = git_blob_rawcontent(blob);
		size_t size = git_blob_rawsize(blob);
		size_t scan_size = size > 4096 ? 4096 : size;
		
		const char *p = content;
		const char *end = content + scan_size;
		
		size_t summary_len = 0;
		int seen_non_header = 0;
		int capturing_summary = 0;

		/* extract title, summary, image */
		while (p < end) {
			const char *eol = memchr(p, '\n', end - p);
			if (!eol) eol = end;
			size_t len = eol - p;

			if (len >= 2 && p[0] == '#' && p[1] == ' ' && !title[0]) {
				size_t tlen = len - 2 < sizeof(title) - 1 ? len - 2 : sizeof(title) - 1;
				memcpy(title, p + 2, tlen);
				title[tlen] = '\0';
			}

			/* scan for ![alt](image) */
			if (!image_route[0]) {
				const char *img = memchr(p, '!', len);
				if (img && img + 3 < eol && img[1] == '[') {
					const char *close_bracket = memchr(img, ']', len - (img - p));
					if (close_bracket && close_bracket[1] == '(') {
						const char *close_paren = memchr(close_bracket + 2, ')', eol - close_bracket - 2);
						if (close_paren) {
							size_t ilen = close_paren - (close_bracket + 2);
							if (ilen < sizeof(image_route) - 1) {
								memcpy(image_route, close_bracket + 2, ilen);
								image_route[ilen] = '\0';
							}
						}
					}
				}
			}

			/* Summary Extraction */
			if (!capturing_summary) {
				/* Skip metadata lines starting with % and empty lines */
				if (len > 0 && p[0] != '%' && p[0] != '\r') {
					capturing_summary = 1;
				}
			}

			if (capturing_summary) {
				if (seen_non_header && (len == 0 || p[0] == '\r')) {
					/* Reached the end of the first paragraph after headers */
					/* We set capturing_summary = 2 to mean "done" so we don't restart */
					capturing_summary = 2;
				} else if (capturing_summary == 1 && summary_len < sizeof(summary) - 2) {
					size_t copy_len = len;
					if (summary_len + copy_len >= sizeof(summary) - 1)
						copy_len = sizeof(summary) - 1 - summary_len;
					
					if (summary_len > 0)
						summary[summary_len++] = '\n';
					
					memcpy(summary + summary_len, p, copy_len);
					summary_len += copy_len;
					summary[summary_len] = '\0';

					if (len > 0 && p[0] != '#' &&
					    !(len >= 2 && p[0] == '!' && p[1] == '[') &&
					    !(len >= 4 && !strncmp(p, "<img", 4))) {
						seen_non_header = 1;
					}
				}
			}

			p = eol;
			if (p < end && *p == '\n') p++;
		}
			git_blob_free(blob);

		if (!title[0]) { /* fallback to path base name */
			const char *base = strrchr(path, '/');
			if (base) base++; else base = path;
			lws_snprintf(title, sizeof(title), "%s", base);
			char *ext = strrchr(title, '.');
			if (ext) *ext = '\0';
		}

		jg2_json_purify(pure_title, title, sizeof(pure_title), NULL);
		jg2_json_purify(pure_summary, summary, sizeof(pure_summary), NULL);
		jg2_json_purify(pure_image, image_route, sizeof(pure_image), NULL);

		lwsl_notice("%s: Appending JSON chunk for %s\n", __func__, path);
		CTX_BUF_APPEND("%c\n{ \"name\": \"%s\",",
			       ctx->subsequent ? ',' : ' ', path);
		ctx->subsequent = 1;
		CTX_BUF_APPEND("\"title\": \"%s\",", pure_title);
		CTX_BUF_APPEND("\"summary\": \"%s\",", pure_summary);
		CTX_BUF_APPEND("\"image\": \"%s\"", pure_image);
		CTX_BUF_APPEND("}");

		ctx->tei = lp_to_te(ctx->tei->next, next);
	}

	if (!ctx->tei) {
		lwsl_notice("%s: completed iterating all blog items\n", __func__);
		meta_trailer(ctx, "\n]");
		job_blog_destroy(ctx);
		ctx->meta_last_job = 1;
	} else {
		lwsl_notice("%s: returning 0 but leaving ctx->tei != NULL\n", __func__);
	}

	return 0;
}
