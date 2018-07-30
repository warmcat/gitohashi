## Email md5 cache

The vhost keeps a cache of computed md5s for a certain amount of recently-seen
email addresses, for use with an avatar provider like gravatar.

By default it's 16 hash bins each of depth 16, so the most recent 256 emails
the vhost has seen.

Each time an email is referenced, it is moved to the start of its hash bin;
when the bins are full the least-recently seen email is replaced with a new
reference.
