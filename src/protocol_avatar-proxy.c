/*
 * avatar_proxy - unidirectional proxy
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
 *
 * This isn't a generic proxy.
 *
 * You can request a cache dir be filled by reading things from a specific
 * remote URL base + a path, using an api reachable from the protocol name +
 * vhost instance.  It's the only way to make requests to fill the cache.
 *
 * Then, separately, you can expose the cache dir as a normal read-only mount
 * dir with whatever caching policy you want.
 *
 * Downstream cache consumers can't request things that aren't already in the
 * cache then, removing any worries about being misused to attack the upstream.
 */

#define LWS_DLL
#define LWS_INTERNAL
#include <libwebsockets.h>

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>

struct req {
	struct lws_dll2 next;
	char filepath_temp[256];
	char filepath[192];
	char urlpath[128];
	size_t got; /* bytes of provider response body streamed so far */
	int fd;
	char renamed; /* temp was renamed to the final cache name */
};

struct pss_avatar_proxy {
	struct lws *wsi;
	char path[128];
	int state;
	int tries;
};

struct vhd_avatar_proxy {
	struct lws_context *context;
	struct lws_vhost *vh;
	const char *remote_base /*
				 * the remote URL being proxied, eg,
				 * https://somewhere.com/ ... fetches will only
				 * be made to URLs starting with this plus
				 * whatever path came in on the request
				 */,
		   *cache_dir;

	pthread_mutex_t lock; /* protect the dlls */

	struct lws_dll2_owner owner;
	struct lws_dll2_owner owner_waiting;

	/* size management of the cache dir */

	struct lws_diskcache_scan *dcs;
	lws_sorted_usec_list_t trim_sul;
	uint64_t cache_size_limit;
};

/*
 * Cap on a single provider response body.  Real avatars are a few KB...
 * this is generous, but stops a hostile provider (or a MITM on an
 * http:// remote-base) filling the disk with one huge body faster than
 * the trim can react.
 */
#define AVATAR_MAX_BODY (1 * 1024 * 1024)

/*
 * Ask the diskcache how we should pace the trim scans... this follows
 * the pattern of libjsongit2's cache trim thread, but on a sul on the
 * service thread since avatar caches are small
 */
static void
avatar_trim_sul_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd_avatar_proxy *vhd = lws_container_of(sul,
					struct vhd_avatar_proxy, trim_sul);
	int n, around = 1;

	/* if we're over budget, scan faster */

	if (!lws_diskcache_secs_to_idle(vhd->dcs))
		around = 8;

	for (n = 0; n < around; n++)
		lws_diskcache_trim(vhd->dcs);

	lws_sul_schedule(vhd->context, 0, &vhd->trim_sul, avatar_trim_sul_cb,
			 LWS_USEC_PER_SEC);
}

static const struct lws_protocols protocols[];

static int
__create_waiting_client_request(struct vhd_avatar_proxy *vhd, struct req *r)
{
	struct lws_client_connect_info i;
	lws_parse_uri_t *puri;
	char u[128];
	struct lws *wsi;

	lws_snprintf(r->filepath_temp, sizeof(r->filepath_temp), "%s~%d-%p",
		     r->filepath, (int)getpid(), vhd);

	r->fd = open(r->filepath_temp, O_RDWR | O_TRUNC | O_CREAT, 0600);
	if (r->fd < 0) {
		lwsl_err("%s: unable to open %s: errno %d\n", __func__,
				r->filepath, errno);
		/* r isn't on any list... don't leak it */
		lws_dll2_remove(&r->next);
		free(r);

		return 1;
	}

	memset(&i, 0, sizeof(i));

	i.context = vhd->context;
	i.ssl_connection = LCCSCF_PIPELINE /* | LCCSCF_ALLOW_SELFSIGNED */;

	puri = lws_parse_uri_create(vhd->remote_base);
	if (!puri) {
		lwsl_notice("%s: parse uri %s: failed\n", __func__,
				vhd->remote_base);
		/*
		 * the caller unlinked us from owner_waiting and we never got
		 * onto vhd->owner... clean up like the connect-failure leg
		 * below or the req (open fd + created temp) is orphaned
		 */
		close(r->fd);
		r->fd = -1;
		unlink(r->filepath_temp);
		lws_dll2_remove(&r->next);
		free(r);

		return 1;
	}
	if (!strcmp(puri->scheme, "https"))
		i.ssl_connection |= LCCSCF_USE_SSL;

	i.address = puri->host;
	i.port = puri->port;
	i.host = i.address;
	i.origin = i.address;
	i.method = "GET";
	i.protocol = "avatar-proxy";
	i.path = r->urlpath;
	i.vhost = vhd->vh;
	i.alpn = "http/1.1";

	 /*
	  * the req is bound to the client fetch request if the client connect
	  * initial part below succeeds
	  */
	i.userdata = r;

	lws_dll2_add_head(&r->next, &vhd->owner);

	strncpy(u, r->urlpath, sizeof(u) - 1);
	u[sizeof(u) - 1] = '\0';

	wsi = lws_client_connect_via_info(&i);
	if (wsi) {
		lwsl_debug("%s: requested %s %s:%d %s\n", __func__, puri->scheme,
				i.address, i.port, r->urlpath);
		lws_parse_uri_destroy(&puri);

		return 0;
	}

	lwsl_notice("%s: failed %s %s:%d %s\n", __func__, puri->scheme, i.address,
		    i.port, u);

	lws_parse_uri_destroy(&puri);

	/* wasn't able to get started... destroy req */

	if (r->fd >= 0) {
		close(r->fd);
		unlink(r->filepath_temp);
	}

	lws_dll2_remove(&r->next);
	free(r);

	return 1;
}

static int
create_waiting_client_requests(struct vhd_avatar_proxy *vhd)
{
	if (!vhd->owner_waiting.head)
		return 0;

	pthread_mutex_lock(&vhd->lock); /* ========================= vhd lock */

	/* on the list of waiting requests? */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->owner_waiting.head) {
		struct req *r = (struct req *)p;

		/* switch to the list of active requests */

		lws_dll2_remove(&r->next);

		__create_waiting_client_request(vhd, r);

	} lws_end_foreach_dll_safe(p, p1);

	pthread_mutex_unlock(&vhd->lock); /* --------------------- vhd unlock */

	return 0;
}

/*
 * Unlike a generic proxy, the downstream side cannot make requests to fill
 * the cache directly.
 *
 * This api function is the only way to request "path" from vhd->remote_base
 * (ie, https://myremote.base/path) to appear in vhd->cache_dir, in "flattened"
 * filename form.
 */

enum {
	MENTION_REQUESTED,
	MENTION_FAILED,
	MENTION_EXISTS,
};

/*
 * Called from a threadpool thread context... we need to queue the request and
 * deal with it from the service context
 */

static int
mention(struct lws_protocols *pcol, struct lws_vhost *vh, const char *path)
{
	struct vhd_avatar_proxy *vhd = (struct vhd_avatar_proxy *)
					  lws_protocol_vh_priv_get(vh, pcol);
	int fd, ret = MENTION_REQUESTED;
	char filepath[128];
	struct req *req;

	lws_snprintf(filepath, sizeof(filepath), "%s/%c/%c/%s_avatar",
		     vhd->cache_dir, path[0], path[1], path);

	/* it already exists as a file in the cache? */

	fd = open(filepath, O_RDONLY);
	if (fd >= 0) {
		close(fd);

		return MENTION_EXISTS;
	}

	/* a request is underway already? */

	pthread_mutex_lock(&vhd->lock); /* ========================= vhd lock */

	/* on the list of client fetches? */

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->owner.head) {
		struct req *r = (struct req *)p;

		if (!strcmp(r->filepath, filepath))
			goto done;

	} lws_end_foreach_dll(p);

	/* on the list of waiting requests? */

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->owner_waiting.head) {
		struct req *r = (struct req *)p;

		if (!strcmp(r->filepath, filepath))
			goto done;
	} lws_end_foreach_dll(p);

	/*
	 * we should queue a new request for http client fetch from
	 * lws service context... add it on the vhd's waiting dll and use
	 * cancel service to signal we need service
	 */

	req = malloc(sizeof(*req));
	if (!req) {
		ret = MENTION_FAILED;
		goto done;
	}

	memset(req, 0, sizeof(*req));

	lws_snprintf(req->filepath, sizeof(req->filepath), "%s", filepath);
	lws_snprintf(req->urlpath, sizeof(req->urlpath),
			"/avatar/%s/?s=128&d=retro", path);

	lwsl_debug("%s: queuing fetch of %s %s\n", __func__, req->urlpath,
		    req->filepath);

	lws_dll2_add_head(&req->next, &vhd->owner_waiting);

	lws_cancel_service(vhd->context);

done:
	pthread_mutex_unlock(&vhd->lock); /* --------------------- vhd unlock */

	return ret;
}

static int
callback_avatar_proxy(struct lws *wsi, enum lws_callback_reasons reason,
		      void *user, void *in, size_t len)
{
	struct vhd_avatar_proxy *vhd = (struct vhd_avatar_proxy *)
			      lws_protocol_vh_priv_get(lws_get_vhost(wsi),
						       lws_get_protocol(wsi));
	struct pss_avatar_proxy *pss = (struct pss_avatar_proxy *)user;
	struct req *req = NULL;
	const char *p;
	int fd;

	switch (reason) {

	/* --------------- protocol --------------- */

	case LWS_CALLBACK_PROTOCOL_INIT: /* per vhost */
		lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
					    lws_get_protocol(wsi),
					    sizeof(struct vhd_avatar_proxy));

		vhd = (struct vhd_avatar_proxy *)
			lws_protocol_vh_priv_get(lws_get_vhost(wsi),
						 lws_get_protocol(wsi));

		vhd->context = lws_get_context(wsi);
		vhd->vh = lws_get_vhost(wsi);

		if (lws_pvo_get_str(in, "remote-base", &vhd->remote_base))
			return -1;
		if (lws_pvo_get_str(in, "cache-dir", &vhd->cache_dir))
			return -1;

		/*
		 * Validate the remote base once here rather than discovering
		 * breakage per-request... an unparseable base would otherwise
		 * orphan every mention()'s req (open temp fd on no list)
		 */
		{
			lws_parse_uri_t *puri;

			puri = lws_parse_uri_create(vhd->remote_base);
			if (!puri) {
				lwsl_err("%s: remote-base \"%s\" unparseable\n",
					 __func__, vhd->remote_base);

				return -1;
			}

			if (strcmp(puri->scheme, "http") &&
			    strcmp(puri->scheme, "https")) {
				lwsl_err("%s: remote-base scheme \"%s\" not "
					 "http/https\n", __func__,
					 puri->scheme);
				lws_parse_uri_destroy(&puri);

				return -1;
			}

			lws_parse_uri_destroy(&puri);
		}

		/*
		 * Assumes gitohashi or package install set up the cache
		 * dirs
		 */

		/*
		 * The avatar cache grows one permanent file per distinct
		 * md5 with nothing else bounding it.  Manage the dir with
		 * the same diskcache LRU the JSON cache uses: prepare the
		 * <c>/<c> dir structure, then trim it down to a byte budget
		 * oldest-first from a 1s sul.  Optional "cache-size" pvo is
		 * the budget in bytes (0 = lws' 256MiB default).
		 */
		{
			const char *z;
			uid_t uid;
			gid_t gid;

			if (!lws_pvo_get_str(in, "cache-size", &z))
				vhd->cache_size_limit = (uint64_t)atoll(z);

			/*
			 * Prepare the <c>/<c> dir structure (no-op if it
			 * already exists).  If a privilege-drop uid is
			 * configured, hand the dirs to it; otherwise use
			 * (uid_t)-1 so the chown is a silent no-op rather
			 * than a wall of EPERM errors from an unprivileged
			 * process
			 */
			lws_get_effective_uid_gid(vhd->context, &uid, &gid);
			lws_diskcache_prepare(vhd->cache_dir, 0700,
					      uid ? uid : (uid_t)-1);

			vhd->dcs = lws_diskcache_create(vhd->cache_dir,
							vhd->cache_size_limit);
			if (!vhd->dcs)
				return -1;

			lws_sul_schedule(vhd->context, 0, &vhd->trim_sul,
					 avatar_trim_sul_cb, LWS_USEC_PER_SEC);
		}

		{
			pthread_mutexattr_t attr;


			pthread_mutexattr_init(&attr);
			pthread_mutexattr_settype(&attr,
						  PTHREAD_MUTEX_RECURSIVE);

			pthread_mutex_init(&vhd->lock, &attr);
		}
		break;

	case LWS_CALLBACK_PROTOCOL_DESTROY:
		if (vhd) {
			/* stop the trim sul or it fires on freed memory */
			lws_sul_cancel(&vhd->trim_sul);
			if (vhd->dcs)
				lws_diskcache_destroy(&vhd->dcs);

			pthread_mutex_destroy(&vhd->lock);
		}
		break;

	/* --------------- http server --------------- */

	case LWS_CALLBACK_HTTP:
		lwsl_debug("LWS_CALLBACK_HTTP %p: %s\n", wsi, (char *)in);
		p = (const char *)in;
		if (*p == '/')
			p++;

		if (!p[0] || !p[1])
			return -1;

		/*
		 * The avatar cache only ever contains files named by their
		 * 32-char MD5 hex digest plus "_avatar" (see mention() ->
		 * lws_snprintf in this file, and lib/main.c
		 * md5_to_hex_cstr()).  The URL tail we receive here is
		 * untrusted (lws does not normalize it), so a request like
		 * "/git/avatar/../../etc/passwd" would otherwise be assembled
		 * into "<cache_dir>/./../etc/passwd" and served via
		 * lws_serve_http_file().
		 *
		 * Accept only the exact avatar filename shape.  This also
		 * prevents the mount serving anything else that might live in
		 * a misconfigured shared cache dir, eg raw JSON cache entries
		 * (shape-identical <32hex>-suffix names) if cache-dir is
		 * pointed at the gitohashi cache-base.
		 */
		{
			size_t tl = strlen(p), n;

			if (tl != 32 + 7 || strncmp(p + 32, "_avatar", 7)) {
				lwsl_notice("%s: illegal avatar path \"%s\"\n",
					    __func__, p);

				return -1;
			}

			for (n = 0; n < 32; n++)
				if ((p[n] < '0' || p[n] > '9') &&
				    (p[n] < 'a' || p[n] > 'f')) {
					lwsl_notice("%s: illegal avatar path "
						    "\"%s\"\n", __func__, p);

					return -1;
				}
		}

		lws_snprintf(pss->path, sizeof(pss->path), "%s/%c/%c/%s",
			     vhd->cache_dir, p[0], p[1], p);

		/* fallthru */

	case LWS_CALLBACK_HTTP_WRITEABLE:
		lwsl_debug("%s: LWS_CALLBACK_HTTP_WRITEABLE %s\n", __func__,
			   pss->path);
		if (!pss->path[0])
			return 0;

		fd = open(pss->path, LWS_O_RDONLY);
		if (fd >= 0) {
			close(fd);
			if (lws_serve_http_file(wsi, pss->path, "image/png",
						NULL, 0) < 0)
				return -1;
			return 0;
		}
		pss->tries++;
		if (pss->tries == 8) {
			unsigned char headers[LWS_PRE + 2048],
				*start = &headers[LWS_PRE], *p = start,
				*end = &headers[sizeof(headers) - 1];

			if (lws_add_http_header_status(wsi,
						HTTP_STATUS_NOT_FOUND, &p, end))
				return -1;

			if (lws_finalize_write_http_header(wsi, start, &p, end))
				return -1;

			if (lws_http_transaction_completed(wsi))
				return -1;

			return 0;
		}

		lws_set_timer_usecs(wsi, LWS_USEC_PER_SEC);
		return 0;

	case LWS_CALLBACK_TIMER:
		lwsl_debug("%s: LWS_CALLBACK_TIMER %s\n", __func__, pss->path);
		lws_callback_on_writable(wsi);
		return 0;

	/* --------------- http client --------------- */

	case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
		lwsl_debug("LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP %p\n", wsi);

		/*
		 * The provider controls the response... a 404/500 page must
		 * not become the permanent cached avatar for the md5, so
		 * only cache 2xx bodies
		 */
		{
			int resp = lws_http_client_http_response(wsi);

			if (resp < 200 || resp > 299) {
				lwsl_notice("%s: avatar fetch got HTTP %d, "
					    "not caching\n", __func__, resp);
				req = (struct req *)user;
				goto nope;
			}
		}
		return 0;

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		lwsl_notice("CLIENT_CONNECTION_ERROR: %p: %s\n", wsi,
				in ? (char *)in : "(null)");
		break;

	/* chunks of chunked content, with header removed */
	case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
		lwsl_debug("RECEIVE_CLIENT_HTTP_READ: read %d\n", (int)len);
		req = (struct req *)user;
		if (req) {
			/* cap the provider body (see AVATAR_MAX_BODY) */
			if (req->got + len > AVATAR_MAX_BODY) {
				lwsl_notice("%s: avatar body exceeds %d bytes\n",
					    __func__, AVATAR_MAX_BODY);
				goto nope;
			}

			if (write(req->fd, in, len) != (ssize_t)len)
				goto nope;

			req->got += len;
		}

		return 0; /* don't passthru */

	/* uninterpreted http content */
	case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
		{
			char buffer[1500 + LWS_PRE];
			char *px = buffer + LWS_PRE;
			int lenx = sizeof(buffer) - LWS_PRE;

			req = (struct req *)user;

			if (req && lws_http_client_read(wsi, &px, &lenx) < 0)
				goto nope;
		}
		return 0; /* don't passthru */

	case LWS_CALLBACK_CLIENT_HTTP_DROP_PROTOCOL:
		lwsl_debug("LWS_CALLBACK_CLIENT_HTTP_DROP_PROTOCOL %p\n", wsi);
		req = (struct req *)user;
		if (!req)
			return 0;
		if (req->fd != -1)
			close(req->fd);

		/*
		 * If the fetch failed partway, the temp file (if any) is
		 * still lying around... the only other copy of the content
		 * is the provider's.  Remove it here so failed fetches
		 * can't litter the cache dir
		 */
		if (!req->renamed)
			(void)unlink(req->filepath_temp);

		pthread_mutex_lock(&vhd->lock); /* ================= vhd lock */
		req->fd = -1;
		lws_dll2_remove(&req->next);
		pthread_mutex_unlock(&vhd->lock); /* ------------- vhd unlock */

		free(req);
		lws_set_wsi_user(wsi, NULL);

		return 0;

	case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
		lwsl_debug("LWS_CALLBACK_COMPLETED_CLIENT_HTTP %p\n", wsi);
		req = (struct req *)user;
		if (req && req->fd != -1) {
			if (!rename(req->filepath_temp, req->filepath))
				req->renamed = 1;
			else
				/* the rename failed... the temp remains for
				 * DROP_PROTOCOL to unlink */
				lwsl_notice("%s: rename to %s failed\n",
					    __func__, req->filepath);
		}
		return 0;

	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		if (vhd)
			create_waiting_client_requests(vhd);
		break;

	default:
		break;
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);

	nope:
		/*
		 * the fetch failed partway... only the temp file exists at
		 * this point (the rename to the final name happens at
		 * COMPLETED), so remove that; DROP_PROTOCOL then closes,
		 * unlists and frees the req
		 */
		if (req && !req->renamed)
			(void)unlink(req->filepath_temp);

		return -1;
}

#define LWS_PLUGIN_PROTOCOL_AVATAR_PROXY \
	{ \
		"avatar-proxy", \
		callback_avatar_proxy, \
		sizeof(struct pss_avatar_proxy), \
		4096, \
		0, \
		(void *)mention \
	} \

#if !defined (LWS_PLUGIN_STATIC)

static const struct lws_protocols protocols[] = {
	LWS_PLUGIN_PROTOCOL_AVATAR_PROXY
};

LWS_EXTERN LWS_VISIBLE int
init_protocol_avatar_proxy(struct lws_context *context,
				struct lws_plugin_capability *c)
{
	if (c->api_magic != LWS_PLUGIN_API_MAGIC) {
		lwsl_err("Plugin API %d, library API %d",
			 LWS_PLUGIN_API_MAGIC, c->api_magic);
		return 1;
	}

	c->protocols = protocols;
	c->count_protocols = LWS_ARRAY_SIZE(protocols);
	c->extensions = NULL;
	c->count_extensions = 0;

	return 0;
}

LWS_EXTERN LWS_VISIBLE int
destroy_protocol_avatar_proxy(struct lws_context *context)
{
	return 0;
}
#endif
