# SA-009 — lib-main + conf-gitolite — 2026-09-13 — 2d61554

Units: `lib/main.c` (895, full-read SA-001, re-verified) + `lib/private.h` (595, remaining bulk) + `include/libjsongit2.h` (public api, walked); `lib/conf/gitolite/gitolite3.c` (762) + `common.c` (214) + `scan-repos.c` (159) + `conf/private.h`  |  Tier: 3  |  Tracks: B(sweep) ×2
Track A check: empty (HEAD = ledger commit 2d61554).  Working tree clean
(HEAD 2d6155448fbc9a74d1a61507c99731540d2d1e42).

## Build/entry-point framing

lib-main: vhost/ctx lifecycle (audited SA-001 for the destroy ordering),
cache-base fence, html template reference machinery.  conf-gitolite is
the **private-repo authz boundary**: ACL decisions outsourced to the
`gitolite` utility via a privilege-dropped long-running child process
(pipes + mkstemp'd O_NOFOLLOW temps), results cached in the JSON cache
(`-acl` suffix), repo discovery by repodir scan, invalidation keyed on
the gitolite-admin HEAD oid.  Inputs: operator pvos (acl_user), the
repodir on disk (operator/gitolite-admin-trusted), and the admin repo's
HEAD (gitolite admin pushes).

## Findings

### F-020 [Low/medium] rei list lifetime is protected by the repodir lock on some paths but read under the vhost lock (or unlocked) on others — a gitolite-admin change frees the lwsac under rd->lock racing concurrent readers (UAF read)
- Location: freed under repodir lock: `lib/conf/gitolite/common.c:196-202` (`__jg2_conf_gitolite_admin_head` — `lwsac_free(&rd->rei_lwsac_head)` + `rei_head = NULL` on admin-head change, called with rd->lock held from ctx create / acl checks); readers under **vhost** lock: `lib/job/job.c:237-267` (cache-hash item 8 walks `repodir->rei_head` + rei conf strings, inside the `jg2_ctx_set_job` vh-lock region :360-388) and `lib/job/job.c:141-151` (`__jg2_job_hash_visible_repos`, "requires vhost lock" — the wrong one); unlocked reader: `lib/job/repos.c:32` (`job_repos_start` picks up `ctx->rei` from `rei_head`)
- Reachability: a gitolite-admin push (admin action) changes the admin HEAD → the next ctx create under rd->lock drops the whole rei lwsac and rescans; any worker concurrently computing a repo-affiliated cache key or starting a repos listing dereferences freed lwsac nodes.
- Preconditions / gating: gitolite-admin push timing coinciding with in-flight requests; the 1s admin-head throttle bounds check frequency but not the race window.
- Attack path: worker under vh lock walks `rei_head` (item 8: name + conf strings for the cache key) → service thread under rd lock frees the lwsac + rescans → worker's `lp_to_rei` chain + `jg2_rei_string` reads freed memory → garbage repo info hashed into cache keys / emitted, or a crash.  Same class as F-009 (operator-timing race, read-side UAF) → Low.
- Evidence: the three locking disciplines over the same list (`rd->lock` in acl/meta paths; `vh->lock` in job.c's hash walks with its own comment claiming it suffices; none at the repos-start pickup).
- Impact assessment: admin-gated timing race; UAF read affecting cache-key correctness (a stale/garbage key can serve one repo's cached JSON under another's URL until refs change) or crash a worker.  Low, medium confidence (lock asymmetry code-verified; race not reproduced).
- Remediation sketch: take the repodir lock for the rei walks in job.c (it is cheap and uncontended — cf. meta_header which already does), or refcount the rei lwsac per ctx, or snapshot name+conf into the ctx at create time.

## Closed rechecks (standing items)

1. **SA-001's log-injection recheck — CLOSED (with a version caveat)**:
   URL-derived strings reach logs at main.c:675 (ACL-denied notice),
   protocol_gitohashi.c DROP_PROTOCOL, and blog.c's raw-path notices —
   but lws's URI C0/DEL fence (the F-018 follow-up in lws) rejects raw
   control bytes from request URIs before they reach gitohashi, and
   %-encoded controls stay encoded (nothing decodes them on these
   paths).  Log injection from URL input is therefore fenced **on lws
   builds carrying the URI fence**; against an older system lws without
   it, newline injection into logs remains possible (log-audience
   impact only).  Caveat recorded.
2. **F-006's fence side — CLOSED**: `JG2_CACHE_BASE_MAX` 256 (private.h:76)
   + reject at main.c:286 + `ctx->cache[384]` sizing; the final_name fix
   (10f873e) completed the set.
3. **F-009's reload side — CLOSED**: `__jg2_vhost_reference_html`
   (main.c:88-131) reloads the template per lwsac mtime under the vh
   lock; the worker-side clamps (f09d2b0) fence the OOB — the
   nanosecond torn-window residual stands as recorded in SA-005.

## Checked clean

1. **Auth-name gate** (common.c:43-65): `[.@\-_0-9A-Za-z]` only — closes
   argv injection into the gitolite subprocess (spaces) and cache-key
   '/'/'..' escape; invalid names demote to NULL (deny-by-default path),
   not reject.  `@all` short-circuit applies to the operator-configured
   acl_user (the daemon has no client-auth plumbing; ctx->acl_user is
   NULL from proto-gitohashi).
2. **Subprocess protocol**: privilege-dropped child (setgroups-first,
   fatal setgid/setuid — the hardened block), argv built only from
   code-composed query strings + the charset-gated auth; mkstemp'd
   unpredictable temp unlinked then re-created by the child with
   O_NOFOLLOW (symlink pre-plant and post-unlink races both fenced —
   documented); parent copies temp→output with O_NOFOLLOW 0600; query
   mutex serializes pipe ordering; umask dance around mkstemp (the CID
   505651 fix); execvp-failure unlink gated to /tmp non-`..` paths.
3. **ACL result parser** (gitolite3.c:641-700): chunked line parser with
   `pos == sizeof(si)` fence, `pos >= 7` guard before the suffix
   compare, tab→NUL normalization, only literal `refs/.*` lines admitted
   (partial-ref rules denied by design), unknown repo names skipped.
4. **scan-repos**: `.git`-suffix dirs only, gitolite-admin hidden
   (belt-and-braces with ctx create's refusal), measure-then-fill lwsac
   alloc with the m/m-4/m-3 name accounting exact (matches
   `jg2_rei_string`), per-repo config snapshot via libgit2.
5. **main.c/private.h** (SA-001 shapes re-verified): ctx/vhost create
   bail paths free exactly once; ACL gate precedes repo open;
   gitolite-admin never servable; struct sizes confirm every buffer
   arithmetic used in earlier passes (`cache[384]`, `trie_filepath[512]`,
   `stack[16]`, `last_from_cache[6]`, `hex_oid[64]`).

## Notes (not findings)

- gitolite3.c:646 `fwrite(buf, 1, n, stderr)` dumps every ACL result
  chunk (repo names of the whole repodir + the auth name) to stderr —
  debug leftover; log-audience disclosure only, fold into any future
  logging pass.
- scan-repos `write()` returns unchecked (silent stdin-list truncation,
  functional); `res` from the child is the raw waitpid status, used only
  for logging.
- The gl3 child's execvp PATH is the repo-uid environment —
  operator-trusted by construction.

## Coverage

Full deep-read: `lib/conf/gitolite/gitolite3.c`, `common.c`,
`scan-repos.c`, `conf/private.h` (47); `lib/private.h` remaining bulk
(structs + prototypes); `include/libjsongit2.h` walked (public surface —
no new input paths); main.c re-verified from SA-001.  Both units →
`done`.  Remaining never-units: `email` (Tier 3), then Tier 4.

## Methodology notes

- None.
