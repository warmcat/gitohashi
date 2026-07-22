/*
 * libjg2 - email / md5 hashing
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
#include <libjsongit2.h>

#include <string.h>

/*
 * Maximum number of bytes of an email (or name) we will hash or scan.
 * sig->email / sig->name come from git commit objects and are therefore
 * attacker-controllable (anyone who can push, or any imported history).
 * Without a cap a single crafted commit with a multi-MB author field can
 * be made to burn CPU and memory on every render.
 */
#define JG2_EMAIL_MAX_LEN 256

static int
__email_hash(struct jg2_vhost *vh, const char *email)
{
	unsigned int s = 0;
	size_t n = 0;

	while (n < JG2_EMAIL_MAX_LEN && email[n])
		s += (unsigned char)email[n++];

	return (int)(s % (unsigned int)vh->cfg.email_hash_bins);
}

unsigned char *
email_md5(struct jg2_vhost *vh, const char *email)
{
	struct jg2_email *p, *op = NULL, *oop = NULL, *ne;
	int bin;

	pthread_mutex_lock(&vh->lock); /* ======================== vhost lock */

	bin = __email_hash(vh, email);

	p = vh->bins[bin].first;

	while (p) {
		if (!(strcmp(p->email, email))) {
			/*
			 * every time we get a hit, move him to be "first" in
			 * the linked-list, ie, quickest to find, and farthest
			 * from getting replaced on the end
			 */

			if (vh->cfg.avatar)
				vh->cfg.avatar(vh->cfg.avatar_arg, p->md5);

			if (!op) { /* he is first */
				pthread_mutex_unlock(&vh->lock); /* vh unlock */
				return p->md5;
			}

			op->next = p->next;
			p->next = vh->bins[bin].first;
			vh->bins[bin].first = p;

			pthread_mutex_unlock(&vh->lock); /*----- vhost unlock */
			return p->md5;
		}

		oop = op;
		op = p;
		p = p->next;
	}

	/* if the search failed, we naturally end up at the end of the ll */

	if (vh->bins[bin].count < vh->cfg.email_hash_depth) {
		/* create a new one */

		vh->bins[bin].count++;
		ne = malloc(sizeof(*ne));
		if (!ne) {
			pthread_mutex_unlock(&vh->lock); /*----- vhost unlock */
			return NULL;
		}
	} else {
		/*
		 * Replace the "last" one (op == tail).  Unlink it from the
		 * list first so we can safely move it to the front.  The old
		 * code only unlinked via oop->next and then unconditionally
		 * did "ne->next = first; first = ne;"; if ne was itself the
		 * head (eg email_hash_depth == 1, so op == first and oop ==
		 * NULL), the unlink was skipped and ne->next was set to ne,
		 * creating a self-cycle that hung every subsequent lookup of
		 * the bin.  Handle the head case explicitly.
		 */

		ne = op;

		if (ne == vh->bins[bin].first)
			/* sole node; just clear the list, ne is reused below */
			vh->bins[bin].first = NULL;
		else if (oop)
			oop->next = op->next; /* op->next is NULL, op is tail */
	}

	ne->next = NULL;
	strncpy(ne->email, email, sizeof(ne->email) - 1);
	ne->email[sizeof(ne->email) - 1] = '\0';

	/* adding new (or newly-recycled) at front */
	ne->next = vh->bins[bin].first;
	vh->bins[bin].first = ne;

	vh->cfg.md5_init(vh->md5_ctx);
	/* cap the hashed length to bound work on attacker-controlled input */
	{
		size_t elen = strlen(email);
		if (elen > JG2_EMAIL_MAX_LEN)
			elen = JG2_EMAIL_MAX_LEN;
		vh->cfg.md5_upd(vh->md5_ctx, (unsigned char *)email, elen);
	}
	vh->cfg.md5_fini(vh->md5_ctx, ne->md5);

	if (vh->cfg.avatar)
		vh->cfg.avatar(vh->cfg.avatar_arg, ne->md5);

	pthread_mutex_unlock(&vh->lock); /*--------------------- vhost unlock */

	return ne->md5;
}

int
email_vhost_init(struct jg2_vhost *vh)
{
	size_t s;

	/* adjust defaults left at 0 */

	if (!vh->cfg.email_hash_bins)
		vh->cfg.email_hash_bins = 16;

	if (!vh->cfg.email_hash_depth)
		vh->cfg.email_hash_depth = 16;

	if (!vh->cfg.md5_alloc) {
		/* use the inbuilt default */
		vh->cfg.md5_alloc = jg2_md5_alloc;
		vh->cfg.md5_init = jg2_md5_init;
		vh->cfg.md5_upd = jg2_md5_upd;
		vh->cfg.md5_fini = jg2_md5_fini;
	}

	vh->md5_ctx = vh->cfg.md5_alloc();

	/* allocate the hash bin array */

	s = sizeof(*vh->bins) * vh->cfg.email_hash_bins;
	vh->bins = malloc(s);
	if (!vh->bins)
		return -1;

	memset(vh->bins, 0, s);

	return 0;
}

void
email_vhost_deinit(struct jg2_vhost *vh)
{
	int n;

	if (!vh->bins)
		return;

	for (n = 0; n < vh->cfg.email_hash_bins; n++) {
		struct jg2_email *p = vh->bins[n].first, *p1;

		while (p) {
			p1 = p->next;
			free(p);
			p = p1;
		}
	}

	free(vh->md5_ctx);

	free(vh->bins);
}
