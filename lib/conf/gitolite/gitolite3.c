/*
 * libjsongit2 - gitolite3 config parsing
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
 * For gitolite v3, we outsource ACL decisions to gitolite itself, by running
 * child processes like "gitolite access myrepo myname R" on demand.
 *
 * These are cached in the general cache, so we only have to ask the same
 * question once per session.
 */

#include "../../private.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <pwd.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/types.h>

#define lp_to_rei(p, _n) lws_list_ptr_container(p, struct repo_entry_info, _n)

#define GL3_VERSION_PROBE "/tmp/_gl3_probe_"
#define GL3_TEMP "/tmp/_gl3_q_"

struct gl3_query {
	char q[128];	  	/* the query args, eg access % v-lws R */
	char stdin_path[128];  	/* the file to use as stdin */
	char stdout_path[128]; 	/* the file to use for stdout */
};

/*
 * This forks to create a long-running child process that takes on the uid
 * and gid of the repositories dir.
 *
 * The parent process returns from this immediately after starting the child
 * process.  It's called by the first vhost to be created, at that time the
 * process should still be privileged allowing use to change to an
 * unprivileged user in the fork and then communicate by pipes.
 *
 * HOME env var for that uid is discovered and the environment prepared.
 *
 * It listens on a pipe for requests to ask gitolite, spawns them, and returns
 * the return value on a separate pipe.
 *
 * If signalled, eg by jg2_gitolite3_interface_destroy(), it exits cleanly.
 *
 * Sets jg2_global->gitolite_version to 2 if v2 gitolite detected, or 3 if v3
 * gitolite detected
 *
 *
 * Because starting the 'gitolite' utility has a lot of overhead getting
 * started, it allows you to pass in multiple repos to query a single user's
 * access right with this kind of invocation:
 *
 * $ echo -e "myrepo1\nmyrepo2" | gitolite access % v-lws R
 *
 * This will return one-line-per-repo results like this in a single query, much
 * faster than individual invocations:
 *
 * myrepo1	v-lws	refs/.*
 * myrepo2	v-lws	R any myrepo2 v-lws DENIED by fallthru
 *
 */

int
jg2_gitolite3_interface(struct jg2_global *jg2_global, const char *repodir)
{
	pthread_mutexattr_t attr;
	struct passwd *passwd;
	struct gl3_query q;
	char home[128];
	struct stat s;
	int child;

	lwsl_notice("%s\n", __func__);

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&jg2_global->lock_query, &attr);

	/* get the uid + gid of the repository dir */

	if (stat(repodir, &s)) {
		lwsl_err("%s: Unable to stat %s\n", __func__, repodir);
		return 1;
	}

	/* discover the home dir for the uid */

	passwd = getpwuid(s.st_uid);
	if (!passwd) {
		lwsl_err("%s: unable to find uid %d\n", __func__, s.st_uid);

		return 1;
	}

	/* prepare the environment table with it for our grandchildren */

	strncpy(home, passwd->pw_dir, sizeof(home) - 1);
	home[sizeof(home) - 1] = '\0';

	/*
	 * create one pipe to send in requests, and another to get
	 * completion indication along with an int retcode
	 */

	if (pipe(jg2_global->gl3_pipe) == -1) {
		lwsl_err("%s: Unable to create control pipe\n", __func__);

		return 1;
	}

	if (pipe(jg2_global->gl3_pipe_result) == -1) {
		lwsl_err("%s: Unable to create result pipe\n", __func__);

		return 1;
	}

	/* create the child process that communicates via the pipes */

	child = fork();
	if (child < 0)
		return child;
	if (child) {
		int n, m;
		char buf[10];

		/* parent */

		/* parent doesn't read from control pipe */
		close(jg2_global->gl3_pipe[0]);
		/* parent doesn't write to result pipe */
		close(jg2_global->gl3_pipe_result[1]);

		jg2_global->gl3_child_pid = child;

		lwsl_notice("gl3 if running as uid %d / gid %d on pid %d\n",
				s.st_uid, s.st_gid, child);

		/*
		 * let's figure out if the gitolite config is for v2 or v3...
		 *
		 * v2 has REPO_UMASK and v3 has UMASK... however before that,
		 * v2 doesn't have the 'gitolite' utility.  But we check both
		 * for the case he has v2 repos but has v3 gitolite also
		 * installed.
		 *
		 * The possible outcomes are:
		 *
		 *  a) /tmp/_gh_test absent (no gitolite utility installed): v2
		 *
		 *  b) exists but empty (v3 installed but v2 .gitolite.rc): v2
		 *
		 *  c) exists and contains the v3 umask: v3
		 */
		jg2_gitolite3_blocking_query(jg2_global, "query-rc UMASK", NULL,
					     GL3_VERSION_PROBE);

		n = open(GL3_VERSION_PROBE, O_RDONLY);
		if (n < 0) { /* outcome a */
			lwsl_notice("%s: no gitolite utility: v2\n", __func__);
			jg2_global->gitolite_version = 2;
		} else {
			m = read(n, buf, sizeof(buf));
			if (m <= 0) { /* outcome b */
				lwsl_notice("%s: empty or no result (%d): v2\n",
						__func__, m);
				jg2_global->gitolite_version = 2;
			} else { /* outcome c */
				lwsl_notice("%s: read %d, v3\n", __func__, m);
				jg2_global->gitolite_version = 3;
			}
		}
		if (n != -1)
			close(n);
		unlink(GL3_VERSION_PROBE);

		if (jg2_global->gitolite_version == 2) {
			lwsl_err("%s: requires gitolite3\n", __func__);

			return 1;
		}

		/*  ---- point that parent returns and continues init ---- */

		return 0;
	}

	/* child has his own life now */

	setenv("HOME", home, 1);

	/* child doesn't write to control pipe */
	close(jg2_global->gl3_pipe[1]);
	/* child doesn't read from result pipe */
	close(jg2_global->gl3_pipe_result[0]);

	/* forked child takes on uid + gid from repo dir */

	if (setgid(s.st_gid))
		lwsl_err("setgid: %s\n", strerror(errno));
	if (setuid(s.st_uid))
		lwsl_err("setuid: %s\n", strerror(errno));

	/*
	 * we block until we get an atomic struct asking us to do something
	 * on the communication pipe.  (The original process writing the pipe
	 * must take care of locking to serialize requests so the ordering is
	 * deterministic for queries and replies.)
	 */

	do {
		int pid, res = -9, fd, fdsi,
		    n = read(jg2_global->gl3_pipe[0], &q, sizeof(q));
		char *args[8], *aa;
		unsigned int m;

		if (n < 0 || n != sizeof(q))
			break;

		args[0] = "gitolite";
		q.q[sizeof(q.q) - 1] = '\0';
		q.stdin_path[sizeof(q.stdin_path) - 1] = '\0';
		q.stdout_path[sizeof(q.stdout_path) - 1] = '\0';
		aa = q.q;

		/* convert the inline args to an array */

		for (m = 1; m < LWS_ARRAY_SIZE(args) - 1; m++) {
			char *p = strchr(aa, ' ');

			if (!p) {
				args[m++] = aa;
				break;
			}

			*p = '\0';
			args[m] = aa;
			aa = p + 1;
		}
		args[m] = NULL;

		for (n = 0; n < (int)m; n++)
			lwsl_debug("  %d: %s\n", n, args[n]);

		/* we got a request... open the output file */

		fd = open(q.stdout_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (fd < 0) {
			lwsl_err("%s: unable to open stdout fd\n", __func__);

			res = -1;
			n = write(jg2_global->gl3_pipe_result[1], &res,
							  sizeof(res));
			if (n < 0 || n != sizeof(res))
				lwsl_err("%s: unable to write query res %d\n",
						__func__, n);

			continue;
		}

		fdsi = -1;
		if (q.stdin_path[0])
			fdsi = open(q.stdin_path, O_RDONLY);

		/* create the fork for the query */

		pid = fork();
		if (pid < 0) {
			lwsl_err("%s: query fork failed %d\n", __func__, pid);
			close(fd);
			if (fdsi != -1)
				close(fdsi);
			continue;
		}

		if (pid) {

			/* parent */

			if (fdsi != -1)
				close(fdsi);
			close(fd); /* parent doesn't want it */
			waitpid(pid, &res, 0);

			/* child is done, so his copy of fd is also closed */

			lwsl_notice("%s: query result %d\n", __func__, res);

			n = write(jg2_global->gl3_pipe_result[1], &res,
				  sizeof(res));
			if (n < 0 || n != sizeof(res))
				lwsl_err("%s: unable to write query res %d\n",
						__func__, n);

			continue;
		}

		/* child: wire up the result filepath to stdin + stdout */

		if (fdsi != -1 && dup2(fdsi, 0) < 0)
			lwsl_err("%s: stdin dup2 failed\n", __func__);

		if (dup2(fd, 1) < 0)
			lwsl_err("%s: stdout dup2 failed\n", __func__);

		/* do the actual query */

		n = execvp(args[0], (char * const *)args);

		/* we don't come back from execvp unless it failed to spawn */

		if (fdsi != -1)
			close(fdsi);
		close(fd);

		/*
		 * Ugh... we opened this with O_TRUNC / O_CREAT... coverity
		 * thinks we shouldn't be able to unlink it though...
		 */
		if (!strstr(q.stdout_path, "..") &&
		    !strncmp(q.stdout_path, "/tmp/", 5))
			unlink(q.stdout_path);

		/* eg, the executable is not on PATH or not installed */
		lwsl_err("%s: execvp failed: %d\n", __func__, errno);

		exit(99);

	} while (1);

	/* clean up child end of pipes */

	close(jg2_global->gl3_pipe[0]);
	close(jg2_global->gl3_pipe_result[1]);

	exit(0);
}

void
jg2_gitolite3_interface_destroy(struct jg2_global *jg2_global)
{
	int result;

	lwsl_notice("%s\n", __func__);

	/* clean up parent end of pipes */

	close(jg2_global->gl3_pipe[1]);
	close(jg2_global->gl3_pipe_result[0]);

	kill(jg2_global->gl3_child_pid, SIGTERM);
	waitpid(jg2_global->gl3_child_pid, &result, 0);
}

/*
 * Request output from the v3 gitolite utility with args from 'query' is stored
 * in the filepath in 'output'.  Block until it is complete.
 *
 * Holds the query mutex to enforce serialization of requests from different
 * threads.
 *
 * If the execution failed, the output file will have been unlinked, otherwise
 * if no other problems it contains the stdout of the process execution.
 */
int
jg2_gitolite3_blocking_query(struct jg2_global *jg2_global, const char *query,
			     const char *stdin_path, const char *output)
{
	int ret = 0, res = -1, fd, fd1, n;
	struct gl3_query q;
	char buf[512], temp[32];

	pthread_mutex_lock(&jg2_global->lock_query); /* === global query lock */

	memset(&q, 0, sizeof(q));

	/*
	 * we have to work around a permissions issue... the fork that queries
	 * gitolite3 runs under gitolite uid + gid.  So it won't be able to
	 * create files in gitohashi / libjsongit2 cache dir under apache:apache
	 * (usually) permissions.  So we ask the fork to create the result in a
	 * temp file, and copy it in to the final cache file here in
	 * libjsongit2.
	 */

	lws_snprintf(temp, sizeof(temp), "/tmp/_gl3q_%d", getpid());
	unlink(temp);
	strncpy(q.stdout_path, temp, sizeof(q.stdout_path) - 1);
	q.stdout_path[sizeof(q.stdout_path) - 1] = '\0';

	if (stdin_path) {
		strncpy(q.stdin_path, stdin_path, sizeof(q.stdin_path) - 1);
		q.stdin_path[sizeof(q.stdin_path) - 1] = '\0';
	} else
		q.stdin_path[0] = '\0';

	strncpy(q.q, query, sizeof(q.q) - 1);
	q.q[sizeof(q.q) - 1] = '\0';

	lwsl_info("starting query %s to %s\n", query, output);

	if (write(jg2_global->gl3_pipe[1], &q, sizeof(q)) != sizeof(q)) {
		lwsl_err("%s: control pipe write failed\n", __func__);
		ret = -2;
		goto bail;
	}

	/* synchronize with the running process */

	if (read(jg2_global->gl3_pipe_result[0], &res, sizeof(res)) !=
						       sizeof(res)) {
		lwsl_err("%s: return pipe read failed\n", __func__);
		ret = -3;
		goto bail;
	}

	/*
	 * if the GL3_TEMP version was created (with gitolite3 permissions),
	 * copy it into the actual output file (with gitohashi permissions)
	 */
	fd = open(temp, O_RDONLY);
	if (fd >= 0) {
		fd1 = open(output, O_CREAT | O_WRONLY | O_TRUNC, 0600);
		if (fd1 >= 0) {
			do {
				n = read(fd, buf, sizeof(buf));
				if (n > 0)
					write(fd1, buf, n);
				else
					break;
			} while (1);
			close(fd1);
		} else
			lwsl_notice("%s: can't open %s\n", __func__, output);
		close(fd);
		unlink(temp);
	} else
		lwsl_notice("%s: can't open %s\n", __func__, temp);

	lwsl_notice("%s: query result %d\n", __func__, res);

bail:
	pthread_mutex_unlock(&jg2_global->lock_query); /* global query unlock */

	return ret;
}

/*
 * Given the gitolite user name, this attaches the acl name as valid to a list
 * of repos the user has R permissions on.
 */

/* must hold repodir lock */

int
__jg2_gitolite3_get_repodir_acls(struct jg2_ctx *ctx, const char *auth)
{
	char cache[128], buf[128], req[128], si[128], *p = NULL;
	struct jg2_repodir *rd = ctx->vhost->repodir;
	int fd, n, m, pos;
	struct aclv3 *a3;

	if (!auth)
		return 1;

	n = __jg2_cache_query_v(ctx, 0, "acl", &fd, cache, sizeof(cache) - 1,
				"acl-%s-%s-%s", rd->repo_base_dir, auth,
				rd->hexoid_gitolite_conf);

	lwsl_notice("%s: %s %s: result %d\n", __func__, rd->repo_base_dir,
						auth, n);

	if (n == LWS_DISKCACHE_QUERY_EXISTS)
		/*
		 * so... we have it in cache and fd is open on the cache file,
		 * we don't have to do any work.  Just judge what's in the file.
		 */
		goto judge;

	if (n == LWS_DISKCACHE_QUERY_NO_CACHE) {
		/*
		 * Cache is disabled.  We can't store the result but we need
		 * to do the query flow.  Direct it to a discardable temp file.
		 */
		lws_snprintf(cache, sizeof(cache) - 1,
			     "/tmp/gitohashi-gl3-query-%d", getpid());
	} else
		close(fd); /* we will reopen it in the forked process */

	/* let's ask gitolite about it... scan-repos prepares us a list of
	 * repos we can use in the format the 'gitolite' utility wants as
	 * stdin to do multiple queries at once... */

	lws_snprintf(si, sizeof(si), "/tmp/_goh_rl_%s",
		     rd->hexoid_gitolite_conf);

	lws_snprintf(req, sizeof(req), "access %% %s R", auth);
	if (jg2_gitolite3_blocking_query(rd->jg2_global, req, si, cache)) {
		/* pas bon */

		return 1;
	}

	/*
	 * We just populated the cache for this... remove the ~suffix that
	 * indicates we were in progress creating it, and rename it to something
	 * that can be found by future queries.
	 */
	lws_diskcache_finalize_name(cache);

	fd = open(cache, O_RDONLY);
	if (fd < 0) {
		lwsl_notice("%s: %s doesn't exist\n", __func__, cache);
		/* bu hao */

		return 1;
	}

judge:

	/* prepare the single copy of the auth name the other items point to */

	p = lwsac_use(&rd->rei_lwsac_head, strlen(auth) + 1, 0);
	if (!p) {
		close(fd);
		return 1;
	}
	strcpy(p, auth);

	/*
	 * We have the result of asking gitolite about the acl in fd, which
	 * is either in a cache file or in a /tmp file just generate by
	 * gitolite.  It contains one-per-line results about R accessability for
	 * the user for each repository in the repodir.
	 *
	 * From gl3 src/triggers/post-compile/update-git-daemon-access-list:
	 *
	 * # As a quick recap, the gitolite output looks somewhat like this:
	 * #
	 * #   bar^Idaemon^IR any bar daemon DENIED by fallthru$
	 * #   foo^Idaemon^Irefs/.*$
	 * #   fubar^Idaemon^Irefs/.*$
	 * #   gitolite-admin^Idaemon^IR any gitolite-admin daemon DENIED by \
	 * #							fallthru$
	 * #   testing^Idaemon^Irefs/.*$
	 * #
	 * # where I've typed "^I" to denote a tab.
	 *
	 * So as we ignore the ref filtering atm, we simply look for
	 * for ^Irefs/.*$ ... acls with partial refs are not served for safety.
	 */

	pos = 0;
	n = 0;
	m = 0;
	do {
		if (m == n) {
			n = read(fd, buf, sizeof(buf) - 1);
			if (n <= 0)
				break;
			fwrite(buf, 1, n, stderr);
			m = 0;
		}
		if (buf[m] == '\n') {
			m++;

			si[pos] = '\0';
			lwsl_notice("%s: %s '%s'\n", __func__, si, si + pos - 7);
			if (!strcmp(si + pos - 7, "refs/.*")) {
				struct repo_entry_info *rei;

				/*
				 * valid... find the rei
				 */

				rei = __jg2_repodir_repo(rd, si);
				if (!rei) {
					lwsl_err("%s: unknown rei %s\n",
							__func__, si);
			//		close(fd);

			//		return 1;
					pos = 0;
					continue;
				}

				lwsl_notice("%s: auth %s valid for %s\n",
						__func__, p, si);

				a3 = lwsac_use(&rd->rei_lwsac_head, sizeof(*a3), 0);
				if (!a3) {
					close(fd);
					return 1;
				}

				a3->acl = p;
				a3->next = rei->acls_valid_head;
				rei->acls_valid_head = a3;
			}

			pos = 0;
			continue;
		}

		si[pos++] = buf[m++];
		if (pos == sizeof(si))
			break;

		if (si[pos - 1] == '\t')
			si[pos - 1] = '\0';

	} while (1);

	close(fd);

	/* mark the rd as having broadside results already for this auth */

	a3 = lwsac_use(&rd->rei_lwsac_head, sizeof(*a3), 0);
	if (!a3)
		return 1;

	a3->acl = p;
	a3->next = rd->acls_known_head;
	rd->acls_known_head = a3;

	return 0;
}

/* must hold repodir lock */

int
__jg2_conf_ensure_acl(struct jg2_ctx *ctx, const char *acl)
{
	struct jg2_repodir *rd = ctx->vhost->repodir;
	struct aclv3 *a;

	__jg2_conf_gitolite_admin_head(ctx);

	a = rd->acls_known_head;
	while (a) {
		if (!strcmp(acl, a->acl))
			/* we already confirmed this acl, check valid list */
			return 0;
		a = a->next;
	}

	/* we didn't check this acl vs the whole repodir yet */

	return __jg2_gitolite3_get_repodir_acls(ctx, acl);
}

/* must hold repodir lock */

int
__jg2_gitolite3_acl_check(struct jg2_ctx *ctx, struct repo_entry_info *rei,
			  const char *acl)
{
	struct aclv3 *a;

	__jg2_conf_ensure_acl(ctx, acl);

	a = rei->acls_valid_head;
	while (a) {
		if (!strcmp(acl, a->acl))
			/* he's valid */
			return 0;
		a = a->next;
	}

	/* nope */

	return 1;
}

