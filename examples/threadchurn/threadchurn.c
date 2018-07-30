/*
 * threadchurn.c: test app for threadsafe api access
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
 *   jg2-threadchurn /srv/repositores /git/myrepo
 *
 * The example spawns 8 threads each doing 1000 fetches from the urlpath
 * concurrently using the same vhost.
 */

#include <libjsongit2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#define URL_VIRTUAL_PART "/git"

static struct jg2_vhost *vh;
static int count_threads = 16;

static void *
thread_spam(void *d)
{
	struct jg2_ctx_create_args args;
	char **argv = (char **)d;
	const char *mimetype;
	unsigned long length;
	struct jg2_ctx *ctx;
	int budget = 10000, did = 0, tried = 0;
	char etag[36];
	int done;

	do {
		memset(&args, 0, sizeof(args));

		args.repo_path = (const char *)argv[2] +
				 strlen(URL_VIRTUAL_PART);
		args.mimetype = &mimetype;
		args.length = &length;
		args.etag = etag;
		args.etag_length = sizeof(etag);

		if (jg2_ctx_create(vh, &ctx, &args)) {
			fprintf(stderr, "failed to open ctx for %s\n", argv[2]);

			break;
		}

		do {
			char buf[4096];
			size_t used;

			done = jg2_ctx_fill(ctx, buf, sizeof(buf), &used);
			if (done < 0) {
				fprintf(stderr, "json job failed\n");

				break;
			}

			/* issue on stdout */
			write(1, buf, used);

		} while (!done);

		if (done > 0)
			did++;

		jg2_ctx_destroy(ctx);


	} while (++tried < budget);

	fprintf(stderr, "thread %p: did %d / %d\n", (void *)pthread_self(),
			did, budget);

	pthread_exit(NULL);

	return NULL;
}

int
main(int argc, char *argv[])
{
	struct jg2_vhost_config config;
	pthread_t pts[16];
	int n;
	void *retval;

	if (argc < 3 || strlen(argv[2]) < strlen(URL_VIRTUAL_PART)) {
		fprintf(stderr, "Usage: %s "
				"<repo base dir> <\"/git/... url path\">\n",
			argv[0]);

		return 1;
	}

	memset(&config, 0, sizeof(config));

	config.virtual_base_urlpath = "/git";
	config.repo_base_dir = argv[1];
	config.acl_user = "@all";

	vh = jg2_vhost_create(&config);
	if (!vh) {
		fprintf(stderr, "failed to open vh\n");

		return 2;
	}

	for (n = 0; n < count_threads; n++)
		if (pthread_create(&pts[n], NULL, thread_spam, argv)) {
			fprintf(stderr, "thread creation failed\n");
		}

	/* wait for the threads to complete */

	for (n = 0; n < count_threads; n++)
		pthread_join(pts[n], &retval);

	jg2_vhost_destroy(vh);

	return 0;
}
