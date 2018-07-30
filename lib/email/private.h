
struct jg2_email {
	struct jg2_email *next;
	char email[64];
	unsigned char md5[JG2_MD5_LEN];
};

struct jg2_email_hash_bin {
	struct jg2_email *first;
	int count;
};

int
email_vhost_init(struct jg2_vhost *vhost);

void
email_vhost_deinit(struct jg2_vhost *vhost);

unsigned char * /* may return NULL on OOM */
email_md5(struct jg2_vhost *vhost, const char *email);
