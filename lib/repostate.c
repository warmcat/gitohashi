/*
 * libjsongit2 - repostate.c
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

#include "private.h"

#include <string.h>

/* vhost lock must be held on entry */

int
__repo_reflist_update(struct jg2_vhost *vh, struct jg2_repo *jrepo)
{
	struct jg2_ref **er_prev = &jrepo->ref_list, *er = *er_prev, *er1,
		       **er_hash_prev[REF_HASH_SIZE];
	unsigned char entry[REF_HASH_SIZE];
	git_reference_iterator *iter_ref;
	int change_seen = 0, n, ret = -1;
	time_t t = time(NULL);
	struct jg2_ctx *ctx;
	git_reference *ref;

	/* limit how often we are willing to do this */

	if (t - jrepo->last_update <= 3)
		return 0;

	pthread_mutex_lock(&jrepo->lock); /* ===================== jrepo lock */

	memcpy(entry, jrepo->md5_refs, sizeof(entry));

	jrepo->last_update = t;

	for (n = 0; n < REF_HASH_SIZE; n++)
		er_hash_prev[n] = &jrepo->ref_list_hash[n];

	/*
	 * First we iterate all the interesting refs and check against our
	 * existing reflist.  Most commonly, they are unchanged and we exit
	 * with 0 indicating no change.
	 *
	 * We keep track of if the only difference was an oid change on an
	 * existing ref.  If so we update it on to go, recompute the repo hash
	 * and we exit with 1, indicating change found.
	 *
	 * If the iterated refs and reflist lose coherence on the names, we
	 * delete and reacquire the whole reflist, recompute the repo hash, and
	 * exit with 2 indicating a change found.
	 *
	 * Because we may mark, eg, commit logs with decorations from any
	 * other ref, if any ref changes all views are potentially affected.
	 */

	if (git_reference_iterator_new(&iter_ref, jrepo->repo) < 0) {
		if (giterr_last())
			giterr_clear();
		goto bail;
	}

	if (giterr_last())
		giterr_clear();

	while (git_reference_next(&ref, iter_ref) >= 0) {
		const char *name = git_reference_name(ref);

		if (!strncmp(name, "refs/heads/", 11) ||
		    !strncmp(name, "refs/tags/", 10)) {
			const git_oid *oid;

			if (!er) { /* lost coherence: extra new refs */
				change_seen |= 2;
				break;
			}

			if (strcmp(name, er->ref_name)) {
				/* lost coherence: wrong name */
				change_seen |= 2;
				break;
			}

			oid = git_reference_target(ref);

			if (git_oid_cmp(&er->oid, oid)) {
				change_seen |= 1; /* existing ref changed oid */
				/* snip us out of existing hash table */
				*er_hash_prev[jg2_oidbin(&er->oid)] =
						er->hash_next;
				/* update our oid */
				git_oid_cpy(&er->oid, oid);
				/* patch us into hash table of the new oid */
				er->hash_next = *er_hash_prev[jg2_oidbin(oid)];
				*er_hash_prev[jg2_oidbin(oid)] = er;
			}
			er_hash_prev[jg2_oidbin(oid)] = &er->hash_next;
			er_prev = &er->next;
			er = er->next;
		}
		git_reference_free(ref);
		ref = NULL;
	}

	if (er) /* lost coherence: less refs than before */
		change_seen |= 2;

	if (!change_seen) {
		git_reference_iterator_free(iter_ref);

		ret = 0;
		goto bail;
	}

	if (!(change_seen & 2))
		/* ref has always been freed if !(change_seen & 2) */
		goto changed1;

	/* snip all the existing refs past the point we lost coherence */

	*er_prev = NULL;
	while (er) {
		/* snip us out of existing hash table */
		*er_hash_prev[jg2_oidbin(&er->oid)] = er->hash_next;
		er1 = er->next;
		jg2_repo_ref_destroy(er);
		er = er1;
	}

	if (!ref)
		/*
		 * we actually handled all the refs.  The only problem was
		 * some left over in the original list...
		 */
		goto changed1;

	/* regenerate the ref list from the point it lost coherence */

	do {
		const char *name = git_reference_name(ref);

		if (!strncmp(name, "refs/heads/", 11) ||
		    !strncmp(name, "refs/tags/", 10)) {
			const git_oid *oid;

			oid = git_reference_target(ref);

			er = jg2_zalloc(sizeof(*er));
			if (!er)
				goto bail;

			er->ref_name = strdup(name);
			git_oid_cpy(&er->oid, oid);

			*er_prev = er;
			er->next = NULL;
			*er_hash_prev[jg2_oidbin(&er->oid)] = er;
			er_hash_prev[jg2_oidbin(&er->oid)] = &er->hash_next;
			er_prev = &er->next;
		}

		git_reference_free(ref);
	} while (git_reference_next(&ref, iter_ref) >= 0);

changed1:
	git_reference_iterator_free(iter_ref);

	/* create a new hash representing all the refs in this repo */

	vh->cfg.md5_init(vh->md5_ctx);

	er = jrepo->ref_list;
	while (er) {
		// lwsl_debug("%s: %s\n", __func__, er->ref_name);

		vh->cfg.md5_upd(vh->md5_ctx, (unsigned char *)er->ref_name,
				   strlen(er->ref_name));
		vh->cfg.md5_upd(vh->md5_ctx, er->oid.id, sizeof(er->oid.id));

		er = er->next;
	}

	vh->cfg.md5_fini(vh->md5_ctx, jrepo->md5_refs);

	{
		char hash33[33];

		md5_to_hex_cstr(hash33, jrepo->md5_refs);

		if (memcmp(entry, jrepo->md5_refs, sizeof(entry)))
			lwsl_notice("%s: %s: ref hash: %s\n", __func__,
				    jrepo->repo_path, hash33);
	}

	/*
	 * Inform all ctx that use this repo about the refchange... this is
	 * useful if the client is on a long poll...
	 */

	ctx = jrepo->ctx_repo_list;
	while (ctx) {
		if (ctx->vhost->cfg.refchange)
			ctx->vhost->cfg.refchange(ctx->user);

		ctx = ctx->ctx_using_repo_next;
	}

	ret = 2;

bail:
	pthread_mutex_unlock(&jrepo->lock); /*------------------ jrepo unlock */

	return ret;
}

int
jg2_vhost_repo_reflist_update(struct jg2_vhost *vhost)
{
	struct jg2_repo *r;
	int m = 0;

	pthread_mutex_lock(&vhost->lock); /* ===================== vhost lock */

	r = vhost->repo_list;

	while (r) {
		m |= __repo_reflist_update(vhost, r);

		r = r->next;
	}

	pthread_mutex_unlock(&vhost->lock); /*------------------ vhost unlock */

	return m;
}
