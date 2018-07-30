/*
 * gitohashi - main.c
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

#include <libwebsockets.h>
#include <libjsongit2.h>

#define LWS_PLUGIN_STATIC
#include "protocol_gitohashi.c"
#include "protocol_avatar-proxy.c"

#include <getopt.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#define LWSWS_CONFIG_STRING_SIZE (32 * 1024)

static int interrupted;

static const struct lws_protocols protocols[] = {
	LWS_PLUGIN_PROTOCOL_GITOHASHI,
	LWS_PLUGIN_PROTOCOL_AVATAR_PROXY,
	{ }
};

static struct lws_context *
context_creation(const char *config_dir)
{
	int cs_len = LWSWS_CONFIG_STRING_SIZE - 1;
	struct lws_context_creation_info info;
	struct lws_context *context;
	char *cs, *config_strings;

	cs = config_strings = malloc(LWSWS_CONFIG_STRING_SIZE);
	if (!config_strings) {
		lwsl_err("Unable to allocate config strings heap\n");

		return NULL;
	}

	memset(&info, 0, sizeof(info));

	info.external_baggage_free_on_destroy = config_strings;
	info.pt_serv_buf_size = 8192;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
		       LWS_SERVER_OPTION_EXPLICIT_VHOSTS |
		       LWS_SERVER_OPTION_VALIDATE_UTF8;

	lwsl_notice("Using config dir: \"%s\"\n", config_dir);

	/*
	 *  first go through the config for creating the outer context
	 */
	if (lwsws_get_config_globals(&info, config_dir, &cs, &cs_len))
		goto init_failed;

	info.pcontext = &context;

	context = lws_create_context(&info);
	if (context == NULL) {
		lwsl_err("lws init failed\n");
		/* config_strings freed as 'external baggage' */
		return NULL;
	}

	info.protocols = protocols;

	if (lwsws_get_config_vhosts(context, &info, config_dir, &cs, &cs_len)) {
		lws_context_destroy(context);

		return NULL;
	}

	return context;

init_failed:
	free(config_strings);

	return NULL;
}

void sigint_handler(int sig)
{
	lwsl_err("signal %d\n", sig);
	interrupted = 1;
}

int main(int argc, const char **argv)
{
	int n = 0, logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_THREAD;
	struct lws_context *context;
	const char *p;

	signal(SIGINT, sigint_handler);

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	lws_set_log_level(logs, NULL);

	lwsl_user("Gitohashi - "
		  "Copyright (C) 2018 Andy Green <andy@warmcat.com>\n");

	context = context_creation("/etc/gitohashi");
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	while (n >= 0 && !interrupted) {
		n = lws_service(context, 1000);
		if (n < 0)
			lwsl_notice("lws_service returned %d\n", n);
	}

	lws_context_destroy(context);

	return 0;
}
