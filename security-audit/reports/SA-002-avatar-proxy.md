# SA-002 — avatar-proxy (+ proto-gitohashi Track A delta) — 2026-09-06 — 696b960

Unit: `src/protocol_avatar-proxy.c` (537 loc)  |  Tier: 1  |  Tracks: B(sweep)
+ Track A delta on `proto-gitohashi` (65d7e7a8 → 696b960)
Build gating: none beyond defaults — compiled statically into the daemon
(`src/main.c` includes it) and as a shared plugin under
`GOH_LWS_PLUGINS=ON`; the example vhost conf mounts it at `/git/avatar`
(`origin: callback://avatar-proxy`), so the server-side surface is
default-exposed.  Protocol requires operator pvos `remote-base` +
`cache-dir` (INIT fails without them).  Working tree clean for the unit
(audited against committed HEAD 696b960dcedb3a05749ad3542044228c7329e780).

## Track A delta: proto-gitohashi fix commits verified

`git log 65d7e7a8..HEAD -- src/protocol_gitohashi.c` = `0669f37` (F-001
fix) + `696b960` (F-002 fix), +15 lines total.  Committed shape re-read at
HEAD: `lws_sul_cancel(&vhd->sul)` first, then `lws_threadpool_finish()` +
`lws_threadpool_destroy()` (join + done-queue reap, each task cleanup
destroying its `jg2_ctx` while the vhost is valid) **before**
`jg2_vhost_destroy()`.  Both fixes sound; ledger row bumped to 696b960.

## Build/entry-point framing (avatar-proxy)

| Entry | Fed by | Trust |
|---|---|---|
| `mention()` (:212, stored as the protocol's `.user`, called via the jg2 `avatar()` cb from protocol_gitohashi.c:306) | **threadpool worker threads**; `path` = 32-char md5 hex of a committer email (`lib/email/email.c:69/:140` always compute `md5_fini` before the cb — input is charset-constrained, never attacker-arbitrary) | repo pusher (chooses the email → chooses the md5) |
| `LWS_CALLBACK_HTTP` (:342) | unauthenticated client: URL tail under `/git/avatar` | **unauthenticated peer** |
| `LWS_CALLBACK_HTTP_WRITEABLE`/`TIMER` (:374/:410) | event loop retry loop (8 tries × 1s) | internal |
| client callbacks `ESTABLISHED/RECEIVE/COMPLETED/DROP/CONNECTION_ERROR` (:417-474) | **the remote avatar provider's HTTP response** (via lws client to `remote-base`) | remote provider / network MITM (http:// remote-base) |
| `__create_waiting_client_request` (:86) | service thread, draining the worker-queued waiting list | internal, operator-config-driven |

Verified supporting contracts (out of unit, witnesses only): the jg2 avatar
cb is invoked with a computed md5 both from the cache-hit path and the
insert path (email.c:55-75, 125-145 — `md5_fini` precedes the cb; the NULL
md5 case exists only in `md5_to_hex_cstr`, which renders `"?"`, an inert
nonexistent filename); lws delivers the mount tail with the query string
already stripped (urlargs live in WSI_TOKEN_HTTP_URI_ARGS), and does not
percent-decode `in` at this layer.

## Findings

### F-003 [Medium/medium] Avatar cache dir grows without bound: no count cap, no per-file cap, no LRU — and the provider's response body is written to disk uncapped
- Location: `src/protocol_avatar-proxy.c:427-433` (`write(req->fd, in, len)` — no cap, no status check), `:261-276` (mention queues a permanent cache entry per distinct md5), `:467-474` (rename to the final name caches whatever the provider sent)
- Reachability: two legs.  (1) **Repo pusher**: every distinct committer email mentioned in rendered JSON (log/tree/commit views) triggers exactly one outbound fetch + one permanent cache file; emails are fully pusher-controlled, so the count is unbounded.  (2) **Provider/MITM**: the response body is streamed to the temp file with no size limit (and no HTTP status check — a 404/500 page becomes the cached "avatar"), so per-file size is provider-controlled; `remote-base` with an `http://` scheme fetches in cleartext (SSL only added for `https`, :113-114), putting a network MITM in the same position.
- Preconditions / gating: default build and default example vhost (`/git/avatar` mount); leg 1 needs push access to any served repo (or one import of foreign history with many author identities), leg 2 needs a malicious/compromised provider or an `http://` remote-base.
- Attack path (leg 1): push commits (or import history) with N synthetic `Author:` emails → view pages that mention those identities → `mention()` (deduped per md5) queues N fetches → N provider requests (outbound amplification against the provider) and N permanent files under `cache-dir/<c>/<c>/<md5>_avatar`; nothing ever reaps or bounds this dir (contrast the JSON cache's `cache-size-limit` + lws_diskcache LRU on the gitohashi protocol).
- Attack path (leg 2): provider (or MITM on http) serves an arbitrarily large body for one avatar URL → `RECEIVE_CLIENT_HTTP_READ` streams it to the temp file until the disk is full (`write()` short/fail then takes the `nope` path, leaking the temp — see F-004).
- Impact assessment: disk exhaustion of the gitohashi host (the JSON cache and repos share the consequence), plus outbound-request amplification.  Gated on repo-push or provider positions rather than the raw anonymous client → Medium.  Medium confidence: the absence of any cap is code-verified; practical exploit velocity depends on provider behavior (normal avatar responses are small, so leg 1 is slow-grow unless automated at scale, leg 2 is immediate under a hostile provider).
- Remediation sketch: cap the streamed body size (fail the fetch past e.g. 1MB), check the response status before `rename()` (don't cache non-2xx), and add a periodic or lazy count/byte budget for the avatar cache dir (the JSON cache's diskcache already demonstrates the pattern); consider refusing non-`https` remote-bases.

### F-004 [Low/high] `nope:` error path unlinks the final name, not the temp — every failed/aborted fetch leaks a partial temp file
- Location: `src/protocol_avatar-proxy.c:487-495` (`(void)unlink(req->filepath);`), temp created at `:93-101` (`r->filepath_temp`), rename only at `:467-474`
- Reachability: any avatar fetch that fails mid-body — provider resets the connection, `write()` errors/short-writes (e.g. disk pressure), or the nope bail from a failed `lws_http_client_read`.
- Preconditions / gating: none beyond defaults; triggered by fetch failures (attacker-influenced volume: every re-queued retry of a failing md5 leaks another temp).
- Attack path: fetch aborts after partial body → `nope:` unlinks `req->filepath` — but only `filepath_temp` (`<final>~<pid>-<vhd>`) exists at that point (rename happens at COMPLETED), so the unlink is a no-op on a nonexistent path → connection drops → `CLIENT_HTTP_DROP_PROTOCOL` (:449-465) closes the fd, removes the req and frees it **without unlinking the temp** → the partial file stays forever.  The `rename()`-failure leg at :471-472 leaks the temp the same way.
- Evidence: `nope: if (req && req->fd != -1) (void)unlink(req->filepath);` — wrong member; DROP_PROTOCOL cleanup has no unlink at all.
- Impact assessment: unbounded-but-slow disk litter in the cache dir, one file per failed attempt, compounding F-003; also self-amplifying under disk pressure (write fails → leak → less disk).  Low severity, high confidence (wrong-variable bug, flow-verified).
- Remediation sketch: unlink `req->filepath_temp` in `nope:` and on the rename-failure leg (or centrally in DROP_PROTOCOL when the temp was not yet renamed — e.g. track "renamed" state on the req).

### F-005 [Low/high] URI-parse failure in `__create_waiting_client_request` orphans the req: open fd + created temp + memory, unreachable by any list
- Location: `src/protocol_avatar-proxy.c:108-112` (`puri` NULL → `return 1`), caller `:174-184` has already `lws_dll2_remove`d the req from `owner_waiting`
- Reachability: requires `remote_base` to fail `lws_parse_uri_create()` — an operator misconfiguration (the pvo is never validated at INIT).
- Preconditions / gating: operator config error; then every drained waiting req leaks (memory + fd + temp file per mention).
- Attack path: misconfigured `remote-base` → first EVENT_WAIT_CANCELLED drains `owner_waiting` → for each req: fd opened, temp created (:96), parse fails, `return 1` before the `owner` add-head at :132 → req on no list → DROP never sees it (no client wsi was created) → leaked for process lifetime.
- Evidence: the early return at :111 precedes both the `lws_dll2_add_head(&r->next, &vhd->owner)` (:132) and the failure cleanup at :151-159 (which only runs for the `lws_client_connect_via_info` failure leg and is correct).
- Impact assessment: bounded by mention rate against a broken config; hygiene-class leak.  Low, high confidence.
- Remediation sketch: validate `remote_base` parseability once at PROTOCOL_INIT (fail loud), and/or give the puri-fail leg the same close/unlink/remove/free cleanup the connect-fail leg has.

## Checked clean

Actively checked against the taxonomy, no findings:

1. **Server-side intake / traversal** (:342-372): the URL tail must be a single path element (`strchr(p, '/')` reject) with `..` rejected; lws does not percent-decode `in` here and strips the query string before delivery, so encoded traversal (`%2f`, `%2e%2e`) stays literal and simply misses; backslash is an inert filename char on POSIX; 2-char minimum prevents degenerate subpaths; `pss->path[128]` composed with `lws_snprintf` (truncation-safe).
2. **`mention()` thread safety** (the SA-001 cross-unit recheck): all dll2 list operations happen under the recursive `vhd->lock`; DROP_PROTOCOL removes the req from the list *under the lock* before freeing it outside the lock, so no lock-holding walker can reach freed nodes; `lws_cancel_service()` is the documented cross-thread service handoff; `lws_protocol_vh_priv_get` is pure pointer math (safe from workers while the vhost lives).  Daemon protocol-table order (gitohashi index 0, avatar-proxy index 1) means the F-001-fixed destroy joins the workers **before** avatar-proxy's `pthread_mutex_destroy` — safe in-tree.
3. **Client-fetch lifecycle on the happy paths**: connect-failure leg closes/unlinks/removes/frees correctly (:151-159); COMPLETED rename guarded by `fd != -1`; `lws_set_wsi_user(wsi, NULL)` after DROP fences late client callbacks (the `req &&` NULL checks pair with it); `lws_parse_uri_destroy` paired on both legs; lws copies address/path during connect so the puri teardown order is fine.
4. **Buffer sizing chain**: `filepath[128]` (mention) → `req->filepath[192]` → `req->filepath_temp[256]` (`~%d-%p` suffix ≈20 bytes — fits); `u[128]` log copy bounded; `urlpath` composition bounded; `write() != len` treated as fatal rather than trusted.
5. **Serving path**: `lws_serve_http_file` on md5-named cache files only, `O_RDONLY`, fixed mimetype (`image/png`) — no header-splitting or content-type injection surface.
6. **PROTOCOL_INIT/DESTROY**: INIT failure paths are fenced by lws's protocol-init bit gate (destroy never fires after failed init, SA-001-verified); pvo `atoi`-free (strings only).

## Notes (not filed)

- **Shutdown-window UB**: at PROTOCOL_DESTROY the dll lists may still hold reqs bound to in-flight client fetch wsis; their later DROP_PROTOCOL locks a mutex that `pthread_mutex_destroy` already destroyed (vhd memory itself is still allocated).  Same shutdown-ordering family as F-001, materially milder (destroyed-mutex lock, glibc-tolerant in practice); a proper fix belongs with any future "drain client fetches at destroy" work.
- **Plugin-embedding order fragility**: the safety of `mention()`-from-workers vs avatar-proxy's mutex destroy relies on the host's protocol table listing gitohashi before avatar-proxy (true for the daemon).  A host that reverses it reintroduces a shutdown race.  Recorded here for the plugin dimension's future.
- **8×1s parked connections**: requests for nonexistent avatar names park the connection ~7s (retry loop) at near-zero CPU — a mild fd-occupancy amplifier, bounded duration; fine unless avatar-mount flooding shows up in practice.
- **Provider error pages cached as avatars** (no status check before rename) — correctness/negative-caching issue folded into F-003's remediation sketch.

## Coverage

Full unit deep-read: `src/protocol_avatar-proxy.c` (all 537 lines).  Witness
reads only: `lib/email/email.c` (avatar cb call sites + md5 ordering),
`etc-gitohashi/conf.d/localhost` (mount shape), `src/protocol_gitohashi.c`
destroy block at HEAD (Track A).  Unit complete → status `done`.

## Methodology notes

- None — dual-track selection worked as designed (small Track A delta
  verified + one Track B unit within the 2-unit budget).
