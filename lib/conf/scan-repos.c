/*
 * libjsongit2 - scan-repos
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

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

#define lp_to_rei(p, _n) list_ptr_container(p, struct repo_entry_info, _n)

static int
rei_alpha_sort(list_ptr a, list_ptr b)
{
	struct repo_entry_info *p1 = lp_to_rei(a, next),
			       *p2 = lp_to_rei(b, next);

	return strcmp((const char *)(p1 + 1), (const char *)(p2 + 1));
}

int
jg2_conf_scan_repos(struct jg2_vhost *vh)
{
	char *name, filepath[256], *p;
	struct repo_entry_info *rei;
	int alen, m, ret = -1;
	git_repository *repo;
	struct dirent *de;
	struct stat s;
	DIR *dir;

	dir = opendir(vh->cfg.repo_base_dir);
	if (!dir) {
		lwsl_err("Unable to walk repo dir '%s'\n",
			 vh->cfg.repo_base_dir);
		return -1;
	}

	pthread_mutex_lock(&vh->lock); /* ======================== vhost lock */

	do {
		de = readdir(dir);
		if (!de)
			break;

		lws_snprintf(filepath, sizeof(filepath), "%s/%s",
			     vh->cfg.repo_base_dir, de->d_name);

		if (stat(filepath, &s))
			continue;

		if (!S_ISDIR(s.st_mode))
			continue;

		m = strlen(de->d_name);
		if (m <= 4 || strcmp(de->d_name + m - 4, ".git"))
			continue;

		/* this cannot be served, so hide it */
		if (!strcmp(de->d_name, "gitolite-admin.git"))
			continue;

		lwsl_notice("%s: %s\n", __func__, filepath);

		if (git_repository_open_ext(&repo, filepath, 0, NULL)) {
			if (giterr_last())
				giterr_clear();
			continue;
		}

		if (giterr_last())
			giterr_clear();

		/* just compute the size of the config elements */
		alen = jg2_get_repo_config(repo, NULL, NULL);

		/* allocate the whole area at once */
		rei = lac_use(&vh->rei_lac_head, sizeof(*rei) + m + alen, 0);
		if (!rei) {
			git_repository_free(repo);
			goto bail;
		}

		/* copy the name in; lac is already advanced + aligned */
		p = name = (char *)(rei + 1);
		memcpy(name, de->d_name, m - 4);
		name[m - 4] = '\0';
		p += m - 3;
		rei->name_len = m - 3;
		rei->acl_len = 0; /* no restriction on this vhost */
		rei->conf_len[0] = rei->conf_len[1] = rei->conf_len[2] = 0;

		/* place the config elements */
		jg2_get_repo_config(repo, rei, p);

		list_ptr_insert(&vh->rei_head, &rei->next, rei_alpha_sort);

		git_repository_free(repo);

	} while (de);

	ret = 0;

bail:
	pthread_mutex_unlock(&vh->lock); /*--------------------- vhost unlock */

	closedir(dir);

	return ret;
}
