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
 */

#include "private.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#define lp_to_fe(p, _n) list_ptr_container(p, struct file_entry, _n)

static const char *hex = "0123456789abcdef";

#define BATCH_COUNT 128

static int
fe_modified_sort(list_ptr a, list_ptr b)
{
	struct file_entry *p1 = lp_to_fe(a, sorted), *p2 = lp_to_fe(b, sorted);

	return p2->modified - p1->modified;
}

/* requires vhost lock: sets ctx->fd_cache and ctx->cache */

int
__jg2_cache_query(struct jg2_ctx *ctx, const char *md5_hex, int *_fd,
		  char *cache, int cache_len)
{
	struct stat s;
	int n;

	/* caching is disabled? */
	if (!ctx->vhost->cfg.json_cache_base)
		return JG2_CACHE_QUERY_NO_CACHE;

	if (!(ctx->flags & JG2_CTX_FLAG_BOT))
		ctx->vhost->cache_tries++;

	n = lws_snprintf(cache, cache_len, "%s/%c/%c/%s",
			 ctx->vhost->cfg.json_cache_base,
			 md5_hex[0], md5_hex[1], md5_hex);

	//lwsl_notice("%s: job cache %s\n", __func__, ctx->cache);

	*_fd = open(cache, O_RDONLY);
	if (*_fd >= 0) {
		int fd;

		if (!(ctx->flags & JG2_CTX_FLAG_BOT))
			ctx->vhost->cache_hits++;

		if (fstat(*_fd, &s)) {
			close(*_fd);

			return JG2_CACHE_QUERY_NO_CACHE;
		}

		ctx->existing_cache_pos = 0;
		ctx->existing_cache_size = (size_t)s.st_size;

		/* "touch" the hit cache file so it's last for LRU now */
		fd = open(cache, O_RDWR);
		if (fd >= 0)
			close(fd);


		return JG2_CACHE_QUERY_EXISTS;
	}

	/* bots are too random to pollute the cache with their antics */
	if (ctx->flags & JG2_CTX_FLAG_BOT)
		return JG2_CACHE_QUERY_NO_CACHE;

	/* let's create it first with a unique temp name */

	lws_snprintf(cache + n, cache_len - n, "~%d-%p", (int)getpid(), ctx);

	*_fd = open(cache, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (*_fd < 0) {
		/* well... ok... we will proceed without cache then... */
		lwsl_notice("%s: Problem creating cache %s: errno %d\n",
			    __func__, cache, errno);
		return JG2_CACHE_QUERY_NO_CACHE;
	}

	return JG2_CACHE_QUERY_CREATING;
}

/*
 * The goal is to collect the oldest BATCH_COUNT filepaths and filesizes from
 * the dirs under the cache dir.  Since we don't need or want a full list of
 * files in there in memory at once, we restrict the linked-list size to
 * BATCH_COUNT entries, and once it is full, simply ignore any further files
 * that are newer than the newest one on that list.  Files older than the
 * newest guy already on the list evict the newest guy already on the list
 * and are sorted into the correct order.  In this way no matter the number
 * of files to be processed the memory requirement is fixed at BATCH_COUNT
 * struct file_entry-s.
 *
 * The oldest subset of BATCH_COUNT files are sorted into the cd->batch
 * allocation in more recent -> least recent order.
 *
 * We want to track the total size of all files we saw as well, so we know if
 * we need to actually do anything yet to restrict how much space it's taking
 * up.
 *
 * And we want to do those things statefully and incrementally instead of one
 * big atomic operation, since the user may want a huge cache, so we look in
 * one cache dir at a time and track state in the repodir struct.
 *
 * When we have seen everything, we add the doubly-linked prev pointers and then
 * if we are over the limit, start deleting up to BATCH_COUNT files working back
 * from the end.
 */

int
jg2_cache_trim(struct jg2_repodir *cd)
{
	size_t cache_size_limit = cd->cache_size_limit;
	char dirpath[132], filepath[132 + 32];
	list_ptr lp, op = NULL;
	int files_trimmed = 0;
	struct file_entry *p;
	int fd, n, ret = -1;
	size_t trimmed = 0;
	struct dirent *de;
	struct stat s;
	DIR *dir;

	if (!cd->cache_subdir) {

		if (cd->last_scan_completed + cd->secs_waiting > time(NULL))
			return 0;

		cd->batch = malloc(sizeof(struct file_entry) * BATCH_COUNT);
		if (!cd->batch) {
			lwsl_err("%s: OOM\n", __func__);

			return 1;
		}
		cd->agg_size = 0;
		cd->head = NULL;
		cd->batch_in_use = 0;
		cd->agg_file_count = 0;
	}

	lws_snprintf(dirpath, sizeof(dirpath), "%s/%c/%c",
		     cd->repo_base_dir, hex[(cd->cache_subdir >> 4) & 15],
		     hex[cd->cache_subdir & 15]);

	dir = opendir(dirpath);
	if (!dir) {
		lwsl_err("Unable to walk repo dir '%s'\n",
			 cd->repo_base_dir);
		return -1;
	}

	do {
		de = readdir(dir);
		if (!de)
			break;

		if (de->d_type != DT_REG)
			continue;

		cd->agg_file_count++;

		lws_snprintf(filepath, sizeof(filepath), "%s/%s", dirpath,
			     de->d_name);

		fd = open(filepath, O_RDONLY);
		if (fd < 0) {
			lwsl_err("%s: cannot open %s\n", __func__, filepath);

			continue;
		}

		n = fstat(fd, &s);
		close(fd);
		if (n) {
			lwsl_notice("%s: cannot stat %s\n", __func__, filepath);
			continue;
		}

		cd->agg_size += s.st_size;

		if (cd->batch_in_use == BATCH_COUNT) {
			/*
			 * once we filled up the batch with candidates, we don't
			 * need to consider any files newer than the newest guy
			 * on the list...
			 */
			if (lp_to_fe(cd->head, sorted)->modified < s.st_mtime)
				continue;

			/*
			 * ... and if we find an older file later, we know it
			 * will be replacing the newest guy on the list, so use
			 * that directly...
			 */
			p = cd->head;
			cd->head = p->sorted;
		} else
			/* we are still accepting anything to fill the batch */

			p = &cd->batch[cd->batch_in_use++];

		p->sorted = NULL;
		strncpy(p->name, de->d_name, sizeof(p->name) - 1);
		p->name[sizeof(p->name) - 1] = '\0';
		p->modified = s.st_mtime;
		p->size = s.st_size;

		list_ptr_insert(&cd->head, &p->sorted, fe_modified_sort);
	} while (de);

	ret = 0;

	cd->cache_subdir++;
	if (cd->cache_subdir != 0x100)
		goto done;

	/* we completed the whole scan... */

	/* if really no guidence, then 256MiB */
	if (!cache_size_limit)
		cache_size_limit = 256 * 1024 * 1024;

	if (cd->agg_size > cache_size_limit) {

		/* apply prev pointers to make the list doubly-linked */

		lp = cd->head;
		while (lp) {
			p = lp_to_fe(lp, sorted);

			p->prev = op;
			op = &p->prev;
			lp = p->sorted;
		}

		/*
		 * reverse the list (start from tail, now traverse using
		 * .prev)... it's oldest-first now...
		 */

		lp = op;

		while (lp && cd->agg_size > cache_size_limit) {
			p = lp_to_fe(lp, prev);

			lws_snprintf(filepath, sizeof(filepath), "%s/%c/%c/%s",
				     cd->repo_base_dir, p->name[0], p->name[1],
				     p->name);

			if (!unlink(filepath)) {
				cd->agg_size -= p->size;
				trimmed += p->size;
				files_trimmed++;
			} else
				lwsl_notice("%s: Failed to unlink %s\n",
					    __func__, filepath);

			lp = p->prev;
		}

		if (files_trimmed)
			lwsl_notice("%s: %s: trimmed %d files totalling "
				    "%lldKib, leaving %lldMiB\n", __func__,
				    cd->repo_base_dir, files_trimmed,
				    ((unsigned long long)trimmed) / KIB,
				    ((unsigned long long)cd->agg_size) / MIB);
	}

	if (cd->agg_size && cd->agg_file_count)
		cd->avg_size = cd->agg_size / cd->agg_file_count;

	/*
	 * estimate how long we can go before scanning again... default we need
	 * to start again immediately
	 */

	cd->last_scan_completed = time(NULL);
	cd->secs_waiting = 1;

	if (cd->agg_size < cache_size_limit) {
		uint64_t avg = 4096, capacity, projected;

		/* let's use 80% of the real average for margin */
		if (cd->agg_size && cd->agg_file_count)
			avg = ((cd->agg_size * 8) / cd->agg_file_count) / 10;

		/*
		 * if we collected BATCH_COUNT files of the average size,
		 * how much can we clean up in 256s?
		 */

		capacity = avg * BATCH_COUNT;

		/*
		 * if the cache grew by 10%, would we hit the limit even then?
		 */
		projected = (cd->agg_size * 11) / 10;
		if (projected < cache_size_limit)
			/* no... */
			cd->secs_waiting  = (256 / 2) * ((cache_size_limit -
						    projected) / capacity);

		/*
		 * large waits imply we may not have enough info yet, so
		 * check once an hour at least.
		 */

		if (cd->secs_waiting > 3600)
			cd->secs_waiting = 3600;
	} else
		cd->secs_waiting = 0;

	lwsl_notice("%s: cache %s: %lldKiB / %lldKiB, next scan %ds\n",
		    __func__, cd->repo_base_dir,
		    (unsigned long long)cd->agg_size / KIB,
		    (unsigned long long)cache_size_limit / KIB,
		    cd->secs_waiting);

	free(cd->batch);
	cd->batch = NULL;

	cd->cache_subdir = 0;

done:
	closedir(dir);

	return ret;
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
				if (!rd->secs_waiting && rd->batch)
					around = 8;

			for (n = 0; n < around; n++)
				jg2_cache_trim(rd);

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
