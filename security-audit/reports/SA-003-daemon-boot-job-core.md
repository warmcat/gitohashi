# SA-003 — daemon-boot + job-core — 2026-09-06 — 696b960

Units: `src/main.c` + `etc-gitohashi/` + `system/` (~150+conf); `lib/job/job.c` + relevant `lib/private.h` fields (1093+ loc)  |  Tier: 1 + 2  |  Tracks: B(sweep) ×2
Track A check: no done unit has commits after its recorded hash
(proto-gitohashi, avatar-proxy both at 696b960 = HEAD) — Track B took the
two lowest `never` units in tier order.  Build gating: default build for
both (job.c is core jsongit2; main.c is the daemon); the conf files are the
shipped example posture.  Working tree clean (HEAD
696b960dcedb3a05749ad3542044228c7329e780; only untracked patches/ + this
audit dir).

## Build/entry-point framing

**daemon-boot**: `main()` → context creation via lws lwsws JSON config
(`/etc/gitohashi`), protocol table = gitohashi + avatar-proxy (static
include), privilege drop to conf uid/gid (48/48 in the example), service
loop, context destroy.  The conf files define the deployment security
posture (CSP headers, TLS, mounts, cache dirs, ACLs).  Trust: config
author = root/operator; the *shipped example* is what most deployments
copy.

**job-core**: `jg2_ctx_fill()` is the worker-thread entry (called from
`task_function` on the threadpool); it sequences the HTML sandwich (meta /
header / JSON jobs / trailer), selects jobs from URL elements, runs the
transparent cache (query/write/finalize), and streams cache hits.
Inputs: URL-derived path elements (repo name, mode, vid, path, search —
via `ctx->sr.e[]`), repo content via the job functions, on-disk cache
files written by earlier requests, and the vhost HTML template
(`vh->html_content` + marker offsets, hot-reloadable).

Verified supporting contracts: `lws_snprintf` returns at most `size`
(and 0 when size==0 — libwebsockets.c:942-958), so `CTX_BUF_APPEND`'s
`p += lws_snprintf(p, end - p, ...)` can never advance `p` past `end`
(the cursor-overshoot class is fenced at the helper level);
`lws_diskcache_query` lays entries at `<base>/<h[0]>/<h[1]>/<full name>`
where name is the unsalted md5 hex (±`-suffix`), and `lws_diskcache_trim`
counts **every** `DT_REG` file under `<c>/<c>/` toward the size limit and
LRU-trims them (diskcache.c:188-189, 311-338); `ctx->cache[384]` sized for
`JG2_CACHE_BASE_MAX`(256) bases; `last_from_cache[6]`.

## Findings

### F-006 [Medium/high] `cache_write_complete` copies the final cache name into `final_name[128]` — stack overflow for the cache-base lengths the code explicitly accepts (92–256 chars)
- Location: `src/…/lib/job/job.c:483-513` (`memcpy(final_name, ctx->cache, n)` at :508-509), base-length fence `lib/main.c:286` (`JG2_CACHE_BASE_MAX` 256, private.h:76)
- Reachability: any vhost operator; fires on the **first completed cache write** (first non-bot cacheable view) — no attacker needed beyond making one request.
- Preconditions / gating: `"cache-base"` pvo with `strlen(base) > 91` (fence accepts up to 256).  The `ctx->cache[384]` buffer and the temp-suffix arithmetic were sized for 256-char bases (the fence exists precisely to support them); `final_name` alone was missed.
- Attack path: base of e.g. 250 chars → cache path `<250>/<c>/<c>/<32hex>~…` fully materialized in `ctx->cache` (diskcache snprintf, cache_len 383) → `n = strchr('~') - cache` = 250+5+32 = 287 → `memcpy(final_name, ctx->cache, 287)` writes 159 bytes past `final_name[128]` (stack, in `cache_write_complete`) → return-address class smash, then `rename()` is attempted on the smashed buffer.
- Evidence: `char final_name[128]` (:485) vs the fence comment at private.h:66-76 computing 71 bytes of structure overhead over a ≤256 base and sizing `ctx->cache` accordingly.
- Impact assessment: stack buffer overflow driven by operator config within the *accepted* input domain — memory corruption in the worker thread on the first cache finalize.  Operator-gated (not remote) → Medium per the table; high confidence (pure arithmetic, code-verified).
- Remediation sketch: size `final_name` like `ctx->cache` (`sizeof(ctx->cache)`), or truncate at `min(n, sizeof-1)` with the rename skipped on mismatch.

### F-007 [Medium/medium] Shipped conf points the avatar cache and the JSON cache at the same directory — the unauthenticated avatar mount then serves raw JSON cache entries, defeating per-vhost repo ACLs for any repo that is public on another vhost
- Location: `etc-gitohashi/conf.d/localhost:119,136` and `conf.d/unixskt` (`"cache-base": "/var/cache/libjsongit2"` + `"cache-dir": "/var/cache/libjsongit2"`), consumed by `src/protocol_avatar-proxy.c:369-387` (serves `<dir>/<p0>/<p1>/<tail>` with only '/'/'..' guards) and `lib/job/job.c:157-296` + `lib/cache.c` (cache key layout)
- Reachability: unauthenticated HTTP client on any vhost with the avatar mount.  The cache key is `md5(job+epoch ‖ count ‖ search ‖ repo-refs-md5 ‖ repo_path ‖ mode ‖ path ‖ oid ‖ repo-conf)` — **unsalted and vhost-independent** for repo-affiliated jobs.
- Preconditions / gating: the shared-dir deployment (exactly what both shipped example vhosts configure) **plus** per-vhost ACL differentiation (exactly what the conf comments recommend: virtual user "v-vhostname" per vhost).  A repo public on vhost A and private on vhost B (same repo-base-dir, shared cache) over the same repo state produces identical keys on both vhosts.
- Attack path: attacker computes the cache key for `repo/tree/…` from vhost A (all hash inputs are public there — same repo, same refs, same repo_path since repodirs are deduped by path) → fetches `GET /git/avatar/<32hex-name>` on vhost A or B → avatar-proxy opens `<shared>/<h0>/<h1>/<32hex>` → `lws_serve_http_file` returns vhost B's cached private-repo JSON (`image/png`).  Names with `-suffix` pass the mount guard too (only '/' and '..' are rejected).
- Evidence: `lws_diskcache_query` path construction (diskcache.c:188-189) is shape-identical to the avatar mount's (`protocol_avatar-proxy.c:369-370`); the hash inputs (job.c:157-296) contain no vhost identity for repo jobs (the no-repo branch hashes visible repos, the repo branch does not).
- Impact assessment: confidentiality bypass of vhost-level repo ACLs (private-repo views: file trees, log metadata, descriptions) to unauthenticated clients — gated on the mixed-ACL + shared-dir config shape rather than a single-vhost default.  Medium, medium confidence (code path fully verified; the exploit needs the mixed-ACL deployment the docs encourage).
- Remediation sketch: give the avatar cache its own directory in the example conf (and document that it must not be the JSON cache base); or namespace avatar files so they cannot be confused with cache entries and restrict the avatar mount to the `_avatar` suffix; hashing a vhost identity into repo-affiliated cache keys would also close the key-equality leg.

### F-008 [Low/high] Raw URL elements (repo name, mode, vid) interpolated into the HTML `<meta Description>` without escaping — HTML injection into the sandwiched page (CSP-fenced to passive content)
- Location: `lib/job/job.c:832-838` (`CTX_BUF_APPEND("<meta name=\"Description\" content=\"…repository: %s, mode: %s, path: %s, rev: %s\">", reponame, mode, …, vid)` — no `ellipsis_purify` on any of them; contrast :600-606 which purifies)
- Reachability: unauthenticated client; the repo must exist (ctx create opens it), but **mode and vid are free-form** — unknown modes keep their raw string through the dispatch (`else` branch at :927-930) and the meta line is emitted before any job runs.
- Preconditions / gating: HTML/sandwich mode (the default `/git` view); active content is fenced by the strict CSP (`script-src 'self'`, no unsafe-inline) — injection is limited to markup/content spoofing (fake UI elements, links) inside the page.
- Attack path: `GET /git/<real-repo>/<payload-mode>/…` where payload-mode contains `"><a href=…>` or similar → HTML_STATE_HTML_META interpolates it raw into the head of the served page → attacker-controlled passive markup in the gitohashi origin.
- Evidence: the same line also contains a copy-paste bug — the "path" slot is filled with `jg2_ctx_get_path(ctx, JG2_PE_NAME, …)` (the repo name again, :836-838), showing the interpolation was never reviewed; every other repo-derived emission in this file goes through `ellipsis_purify`.
- Impact assessment: CSP-gated HTML injection (no script execution; content spoofing only) — Low per the F-021-class precedent; high confidence the interpolation is unescaped.
- Remediation sketch: route all four interpolations through `ellipsis_purify()` like the neighboring meta_header fields (and fix the doubled-NAME slot).

### F-009 [Low/medium] Template hot-reload race: workers read the vhost HTML offsets unlocked with a stale `html_pos` — a shortened/replaced template mid-sandwich wraps the size_t length and memcpys past the template buffer into the response
- Location: `lib/job/job.c:813-820, 846-854, 1075-1083` (`m = ctx->vhost->meta - ctx->html_pos > left ? left : …` with `size_t m`, then `memcpy(ctx->p, vh->html_content + ctx->html_pos, m)`), offsets/pointer updated under the vhost lock on the service thread by `__jg2_vhost_reference_html` (`lib/main.c:88-131`, called from every ctx create — mtime-based hot reload is a documented feature)
- Reachability: vhost operator replaces/edits the HTML template to a shorter one (or one with earlier markers) while requests are in flight — a sandwich spans multiple `jg2_ctx_fill` calls, so a worker re-enters with `html_pos` beyond the new `vh->meta`/`html_len`.
- Preconditions / gating: operator edit during live traffic; window = one in-flight sandwich (fills are fast, but template edits during load make hits routine).
- Attack path: reload shrinks `vh->html_len` below a worker's `html_pos` → `vh->html_len - html_pos` wraps huge (size_t) → `> left` → `m = left` → `memcpy(dst, html_content + html_pos, left)` reads up to a full fill buffer past the lwsac template allocation → OOB bytes are appended to the served page (heap info leak into responses), or a crash.
- Evidence: unlocked worker-side reads of `vh->meta/dynamic/html_len/html_content` (no vhost lock in the HTML states; setters take it) + unsigned wrap arithmetic; same shape in all three HTML states.
- Impact assessment: operator-timing-gated OOB read into client responses.  Low, medium confidence (arithmetic verified; the reload race window is plausible but untested).
- Remediation sketch: clamp `m` against `vh->html_len - html_pos` with a signed comparison bailing to a reset/restart of the sandwich when `html_pos` exceeds the current offsets (or snapshot the content pointer + offsets per-ctx under the lock at first fill).

## F-003 status-history addendum (filed by this pass)

The shared-dir deployment shape (F-007's config) partially mitigates F-003's
disk-growth leg: `lws_diskcache_trim` counts every regular file under
`<c>/<c>/` toward the JSON cache's `cache-size` budget and LRU-trims them,
so avatar files in the shared dir are bounded by the JSON cache limit.
However (a) a separate avatar `cache-dir` (also a plausible reading of the
pvos) remains fully unbounded, (b) the uncapped single-body write leg is
unaffected (one huge provider body outruns the trim cadence), and (c)
trim-evicted `_avatar` files are re-fetched on the next mention — the
outbound amplification leg becomes an unbounded re-fetch cycle.  F-003
stays open with this nuance recorded in its Status history.

## Checked clean

**daemon-boot**: CSP of the shipped conf is strict (default-src 'none',
script/style/font/connect-src 'self', frame-ancestors + base-uri 'none',
nosniff, no-referrer — no unsafe-inline anywhere); privilege drop to
conf uid/gid after init; `LWS_SERVER_OPTION_VALIDATE_UTF8` +
`EXPLICIT_VHOSTS`; JSON config parsing itself is lws lwsws code (audited
lws-side); the 100y example certs are clearly marked examples.  Posture
notes (not findings): SIGHUP is not handled by the daemon (systemd
`ExecReload` = kill → restart-on-failure picks it up, i.e. "reload" is
really a restart); `ExecStop` uses `killall gitohashi` (name-match
killall); the signal flag is not `volatile sig_atomic_t`; no systemd
hardening directives (ProtectSystem/NoNewPrivileges would be free wins).

**job-core**: `CTX_BUF_APPEND`/`lws_snprintf` truncation fence verified at
the lws helper (p can never pass `end`; size-0 returns 0) — the
cursor-overshoot class is absent from the whole file; the
`job_spool_from_cache` 6-byte tail-window arithmetic is exact-boundary safe
(empty file, sub-6-byte files, exact-multiple final reads, the `]}`-strip
fixup stays in-bounds); `cache_write` partial-write failure closes +
unlinks and degrades to uncached generation; job table indexed only by
internal enums; `hex_oid` strncpy bounded; `job_cache_query` read under
the vhost lock; chained-job flags (did_bat/did_sat/inline_filename)
consistent; the error path emits `ctx->status` purified.  SA-001's
cross-unit recheck on `jg2_ctx_fill` honoring the caller bound is
**closed**: `ctx->end = buf + len - 1` + the clamp means `*used ≤ len` and
`p` in-bounds always.

## Coverage

daemon-boot: `src/main.c` (read fully in SA-001 as witness, re-checked),
`etc-gitohashi/conf` + `conf.d/localhost` + `conf.d/unixskt` (full),
`system/gitohashi.service` (full; `gitohashi-selinux.pp` binary, not
audited).  job-core: `lib/job/job.c` all 1093 lines + the ctx fields in
`lib/private.h` (345-419); `lib/job/private.h` was read in SA-001
(158 lines, job-state enums).  F-001's recheck (fill-path vhost derefs)
confirmed in fixed order (SA-002 already verified the destroy side).
Both units → `done`.

## Methodology notes

- None.
