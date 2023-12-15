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

#define lp_to_rei(p, _n) lws_list_ptr_container(p, struct repo_entry_info, _n)

static int
rei_alpha_sort(lws_list_ptr a, lws_list_ptr b)
{
	struct repo_entry_info *p1 = lp_to_rei(a, next),
			       *p2 = lp_to_rei(b, next);

	return strcmp((const char *)(p1 + 1), (const char *)(p2 + 1));
}

/* must have repodir lock
 *
 * We create a file /tmp/_goh_rl_GITOLITE_ADMIN_HEAD_HASH that contains a list
 * of dirs in the repodir (without the .git).
 */

int
__jg2_conf_scan_repos(struct jg2_repodir *rd)
{
	int alen, m, ret = -1, fd = -1, f = 0;
	char *name, filepath[256], *p;
	struct repo_entry_info *rei;
	git_repository *repo;
	struct dirent *de;
	struct stat s;
	DIR *dir;

	if (rd->rei_head) {
		lwsl_notice("%s: NOP since rei head populated\n", __func__);

		return 0;
	}

	dir = opendir(rd->repo_base_dir);
	if (!dir) {
		lwsl_err("Unable to walk repo dir '%s'\n",
			 rd->repo_base_dir);
		return -1;
	}

	lws_snprintf(filepath, sizeof(filepath),
		     "/tmp/_goh_rl_%s", rd->hexoid_gitolite_conf);
	fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) {
		lwsl_err("%s: unable to create repo list: %s\n", __func__, filepath);
		ret = -1;
		goto bail;
	}

	lwsl_err("%s: preparing stdin %s\n", __func__, filepath);

	do {
		de = readdir(dir);
		if (!de)
			break;

		lws_snprintf(filepath, sizeof(filepath), "%s/%s",
			     rd->repo_base_dir, de->d_name);

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
		rei = lwsac_use(&rd->rei_lwsac_head, sizeof(*rei) + m + alen, 0);
		if (!rei) {
			git_repository_free(repo);
			goto bail;
		}

		/* copy the name in; lac is already advanced + aligned */
		p = name = (char *)(rei + 1);
		memcpy(name, de->d_name, m - 4);
		name[m - 4] = '\0';

		lwsl_err("%s: creating rei for %s\n", __func__, name);

		p += m - 3;
		rei->name_len = m - 3;
		rei->acls_valid_head = NULL;
		rei->conf_len[0] = rei->conf_len[1] = rei->conf_len[2] = 0;

		/* place the config elements */
		jg2_get_repo_config(repo, rei, p);

		lws_list_ptr_insert(&rd->rei_head, &rei->next, rei_alpha_sort);

		git_repository_free(repo);

		if (f)
			write(fd, "\n", 1);
		f = 1;
		write(fd, de->d_name, m - 4);

	} while (de);

	ret = 0;

bail:
	if (fd != -1)
		close(fd);
	closedir(dir);

	return ret;
}
