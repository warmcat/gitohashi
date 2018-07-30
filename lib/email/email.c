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

static int
__email_hash(struct jg2_vhost *vh, const char *email)
{
	int s = 0;

	while (*email)
		s += *email++;

	return s % vh->cfg.email_hash_bins;
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
	} else
		/* replace the "last" one */

		ne = op;

	ne->next = NULL;
	strncpy(ne->email, email, sizeof(ne->email) - 1);
	ne->email[sizeof(ne->email) - 1] = '\0';

	if (ne == op && oop) /* replacing existing, just move to front */
		oop->next = op->next; /* should always be NULL */

	/* adding new at front */
	ne->next = vh->bins[bin].first;
	vh->bins[bin].first = ne;

	vh->cfg.md5_init(vh->md5_ctx);
	vh->cfg.md5_upd(vh->md5_ctx, (unsigned char *)email, strlen(email));
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
