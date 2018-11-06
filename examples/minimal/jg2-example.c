/*
 * jg2-example.c: minimal example for using libjsongit2
 *
 * Copyright (C) 2018 Andy Green <andy@warmcat.com>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 *
 * The library is LGPL 2.1... this example is CC0 to ease getting started
 * with your own code using the library.
 *
 * You run it from the commandline with a fake "url", against a bare git dir,
 * and it spits out the corresponding JSON.  You use it like this
 *
 *  - repo base dir
 *  - "url" part to give the library
 *
 * This example just sets the virtual url part to "/git", this is pass in the
 * json to whatever will create links.  The "/git" part is snipped before
 * passing it to the library.
 *
 *   jg2-example /srv/repositores /git/myrepo
 *   jg2-example /srv/repositores /git/myrepo/commit?id=somehash
 *   jg2-example /srv/repositores /git/myrepo/log?h=mybranch
 */

#include <libjsongit2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define URL_VIRTUAL_PART "/git"

int
main(int argc, char *argv[])
{
	struct jg2_ctx_create_args args;
	struct jg2_vhost_config config;
	struct jg2_vhost *vh;
	struct jg2_ctx *ctx;
	const char *mimetype;
	unsigned long length;
	int done, err = 1;
	char etag[36];

	if (argc < 3 || strlen(argv[2]) < strlen(URL_VIRTUAL_PART)) {
		fprintf(stderr, "Usage: %s "
				"<repo base dir> <\"/git/... url path\">\n",
			argv[0]);

		return 1;
	}

	memset(&config, 0, sizeof(config));

	/*
	 * this is the "virtual" part of the URLs, we just fix it to "git"
	 * for the sake of a simple example.
	 */
	config.virtual_base_urlpath = "/git";

	/*
	 * this is the directory where the bare repos are living,
	 * eg, /srv/repositories or whatever.  The repository name
	 * from the URL is added to this to find out the actual repo to
	 * open.  We pass it in the first arg
	 */
	config.repo_base_dir = argv[1];

	/*
	 * typically the vhost has its own "user" for gitolite ACL matching.
	 *
	 * For the demo we just allow it to see all repos with this special
	 * macro name.
	 */
	config.acl_user = "@all";

	/*
	 * the jg2 vhost holds things like different repo objects, information
	 * from the config set above, and universal cache for email + md5s.  In
	 * a more complex usage, it exists for longer than one context lifetime.
	 */
	vh = jg2_vhost_create(&config);
	if (!vh) {
		fprintf(stderr, "failed to open vh\n");

		return 2;
	}

	/*
	 * We create a jg2 context in the jg2 vhost, using the "url"
	 * from the commandline like "/git/myrepo".  Its lifetime is until
	 * we finished generating that specific JSON.
	 *
	 * We snip the "/git" part simulating how a web server would have
	 * removed the virtual part of the URL
	 */

	/*
	 * This makes your code more forward-compatible, since NULL / 0 is
	 * almost always meaning "not used" or "default" for members introduced
	 * in later versions than your code was written with.
	 */
	memset(&args, 0, sizeof(args));

	args.repo_path = (const char *)argv[2] + strlen(URL_VIRTUAL_PART);
	args.mimetype = &mimetype;
	args.length = &length;
	args.etag = etag;
	args.etag_length = sizeof(etag);

	if (jg2_ctx_create(vh, &ctx, &args)) {
		fprintf(stderr, "failed to open ctx for %s\n", argv[2]);

		goto bail;
	}

	do {
		char buf[4096];
		size_t used;

		/* the context is stateful, we don't have to do this all at
		 * once and we can use a different buffer every time if we
		 * want.
		 */

		done = jg2_ctx_fill(ctx, buf, sizeof(buf), &used, NULL);
		if (done < 0) {
			fprintf(stderr, "json job failed\n");

			goto bail1;
		}

		/* issue on stdout */
		write(1, buf, used);

	} while (!done);

	err = 0;

bail1:
	jg2_ctx_destroy(ctx);

bail:
	jg2_vhost_destroy(vh);

	return err;
}
