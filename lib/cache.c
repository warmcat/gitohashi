/*
 * libjsongit2 - cache trimming
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
 *  This is a thin wrapper on the lws_diskcache_ api in lws.
 */

#include "private.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

/*
 * this lets you hash the vargs and then if non-NULL, add a suffix to the
 * hex md5 chars, and do the cache query flow.  The suffix acts like a namespace
 * on the hash, since cached objects with different suffices can never collide.
 */

int
__jg2_cache_query_v(struct jg2_ctx *ctx, int flags, const char *suffix,
		    int *_fd, char *cache, int cache_len, const char *format,
		    ...)
{
	va_list ap;
	char buf[256], md5_hex[33], *p = md5_hex;
	int n, l = 0;
	jg2_md5_context md5_ctx;
	unsigned char md5[JG2_MD5_LEN];

	if (suffix)
		l = strlen(suffix);

	va_start(ap, format);
	n = vsnprintf(buf, sizeof(buf) - l - 1, format, ap);
	va_end(ap);

	md5_ctx = ctx->vhost->cfg.md5_alloc();
	if (!md5_ctx)
		return LWS_DISKCACHE_QUERY_NO_CACHE;
	ctx->vhost->cfg.md5_init(md5_ctx);
	ctx->vhost->cfg.md5_upd(md5_ctx, (unsigned char *)buf, n);
	ctx->vhost->cfg.md5_fini(md5_ctx, md5);

	free(md5_ctx);

	md5_to_hex_cstr(md5_hex, md5);

	if (suffix) {
		lws_snprintf(buf, sizeof(buf) - 1, "%s-%s", md5_hex, suffix);
		p = buf;
	}

	ctx->existing_cache_pos = 0;
	return lws_diskcache_query(ctx->vhost->cachedir->dcs, flags, p, _fd,
				   cache, cache_len, &ctx->existing_cache_size);
}



/*
 * Check every base cache dir incrementally so it completes over 256s, one dir
 * for each base cache dir per second.
 *
 * The first time we see a cache dir though, do it all at once immediately.
 */

void *
cache_trim_thread(void *d)
{
	struct jg2_global *jg2_global = (struct jg2_global *)d;
	struct jg2_vhost *vh;

	sleep(2); /* wait for other vhosts that might set size limit */

	jg2_safe_libgit2_init();

	while (jg2_global->count_cachedirs) {
		struct jg2_repodir *rd = jg2_global->cachedir_head;

		while (rd) {
			int n, around = 1;

			if (!rd->subsequent)
				around = 256;
			else
				/* are we over the limit?  Let's speed up */
				if (!lws_diskcache_secs_to_idle(rd->dcs))
					around = 8;

			for (n = 0; n < around; n++)
				lws_diskcache_trim(rd->dcs);

			rd->subsequent = 1;

			rd = rd->next;
		}

		sleep(1);

		pthread_mutex_lock(&jg2_global->lock); /* ======= global lock */

		vh = jg2_global->vhost_head;
		while (vh) {
			jg2_vhost_repo_reflist_update(vh);
			vh = vh->vhost_list;
		}

		pthread_mutex_unlock(&jg2_global->lock); /* --- global unlock */
	}

	jg2_safe_libgit2_deinit();

	pthread_exit(NULL);

	return NULL;
}

int
cache_trim_thread_spawn(struct jg2_global *jg2_global)
{
	if (pthread_create(&jg2_global->cache_thread, NULL,
			   cache_trim_thread, jg2_global))
		return 1;

#if defined(JG2_HAS_PTHREAD_SETNAME_NP)
	pthread_setname_np(jg2_global->cache_thread, "cache-trim");
#endif

	return 0;
}
