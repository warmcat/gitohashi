# SA-001 — proto-gitohashi — 2026-09-06 — 65d7e7a

Unit: `src/protocol_gitohashi.c` (794 loc)  |  Tier: 1  |  Tracks: B(sweep)
Build gating: none beyond the project defaults — the daemon target compiles
this file directly (`src/main.c` does `#define LWS_PLUGIN_STATIC` +
`#include "protocol_gitohashi.c"`), and the `GOH_LWS_PLUGINS=ON` dimension
builds the same source as a shared plugin.  Everything in this unit is
reachable in the default build, on the standard vhost config (mount with the
`gitohashi` protocol).  Working tree clean for the unit (audited against
committed HEAD 65d7e7a839f1d332f9be3b8bd9f0a0a59b7206e3; untracked
`patches-*/` dirs are stgit series storage, not sources).

## Build/entry-point framing

`callback_gitohashi` is the http protocol handler; `task_function` is the lws
threadpool worker entry.  Entry points and trust levels:

| Entry | Fed by | Trust |
|---|---|---|
| `LWS_CALLBACK_HTTP` (protocol_gitohashi.c:466) | lws http server: `in` = URL after mountpoint, URI args + UA/Accept-Language/If-None-Match/Cache-Control/Pragma headers | **unauthenticated peer** |
| `LWS_CALLBACK_HTTP_WRITEABLE` (:652) | event loop after threadpool SYNC | internal handshake |
| `task_function` (:75) | threadpool worker thread | internal; operates on peer-requested work (repo content via jg2) |
| `http_reply` (:143, service thread) | called from WRITEABLE | internal |
| `avatar()` (:306) | jg2 library from **worker threads** | repo-content-derived md5s |
| `refchange()` (:268) | jg2 from worker threads | internal (currently a no-op logger) |
| `LWS_CALLBACK_PROTOCOL_INIT`/`DESTROY` | vhost lifecycle; pvos are operator config | operator |

Verified supporting contracts (out of unit, for attack-path completeness):
`lws_hdr_copy_fragment` copies nothing and returns -2 when a fragment does
not fit (lib/roles/http/parsers.c:532); `lws_hdr_copy` returns -1 on
overflow (copies nothing but a NUL) (parsers.c:551); `lws_threadpool_destroy`
joins the workers and then runs each done-task's cleanup
(threadpool.c:865-881); `lws_threadpool_finish` marks the pool destroying and
STOPPED-moves the pending queue but does not wait (threadpool.c:796); lws
context destroy closes all wsi **before** `__lws_vhost_destroy2` delivers
PROTOCOL_DESTROY (lib/core/context.c destroy sequence), and lws only delivers
PROTOCOL_DESTROY to protocols whose PROTOCOL_INIT succeeded
(vhost.c:1725 `vh->protocol_init` bit gate).

## Findings

### F-001 [Low/high] PROTOCOL_DESTROY frees the jg2 vhost (incl. git_libgit2_shutdown) before joining threadpool workers still inside jg2_ctx_fill — shutdown-window UAF
- Location: `src/protocol_gitohashi.c:453-462` (destroy order), fed by `lib/main.c:437-520` (`jg2_vhost_destroy`) and dereferenced from `lib/job/job.c:140-364` (`ctx->vhost` throughout `jg2_ctx_fill`) and `lib/main.c:886` (`jg2_ctx_destroy` takes `vh->lock`)
- Reachability: any threadpool task still executing at daemon/vhost teardown.  An unauthenticated client can deliberately hold a long task open (e.g. blame on a large file in a public repo — expensive by design, cf. the blame-shedding commit 02df748); systemd restart/admin stop closes the window.
- Preconditions / gating: none beyond defaults; the window is daemon shutdown or (plugin build, `GOH_LWS_PLUGINS=ON`) any embedding host that destroys a vhost at runtime.
- Attack path:
  1. Worker is inside `task_function` → `jg2_ctx_fill(priv->ctx, ...)` (long git blame / log walk).
  2. Daemon shutdown: lws closes wsis (task told STOPPING — but a worker *currently executing* keeps running until the fill returns), then delivers PROTOCOL_DESTROY.
  3. `jg2_vhost_destroy` runs first: `git_repository_free()` on every repo, `jg2_safe_libgit2_deinit()` → `git_libgit2_shutdown()` under active libgit2 users, `free(vhost)`.
  4. The still-running worker's fill path dereferences freed state: `ctx->vhost->cfg.md5_*` function pointers, `ctx->vhost->repodir`, `vhost->lock`, `cachedir` (job.c:140-364) — heap UAF read/call through freed memory.
  5. Then `lws_threadpool_destroy` joins the workers and runs done-task cleanups (`cleanup_task_private_data` → `jg2_ctx_destroy(priv->ctx)` → `pthread_mutex_lock(&vh->lock)` on the freed+mutex_destroyed vhost, main.c:886) — a second UAF leg after the free.
- Evidence: `jg2_vhost_destroy(vhd->jg2_vhost); lws_threadpool_finish(vhd->tp); lws_threadpool_destroy(vhd->tp);` (protocol_gitohashi.c:458-460) — finish does not wait, and destroy's join happens only after the vhost free; `jg2_ctx_destroy` unconditionally locks `ctx->vhost->lock` (main.c:884-892).
- Impact assessment: UAF-class memory corruption confined to the shutdown window of a process that is exiting anyway (crash, unclean stop under systemd).  Confidentiality/integrity impact not shown; exploitability during the window is contrived.  Hence Low, not Medium — but the fix is a trivial reorder.
- Remediation sketch: in PROTOCOL_DESTROY, run `lws_threadpool_finish()` + `lws_threadpool_destroy()` (which joins workers and reaps/cleans all tasks, destroying their ctxs while the vhost is still valid) **before** `jg2_vhost_destroy()`.

### F-002 [Info/medium] dump_cb 1s sul is never cancelled at PROTOCOL_DESTROY — UAF + self-reschedule loop if a vhost is destroyed while its context lives
- Location: `src/protocol_gitohashi.c:328-338` (dump_cb), scheduled at :450, no `lws_sul_cancel(&vhd->sul)` in PROTOCOL_DESTROY (:453-462)
- Reachability: requires a vhost destroy with the context still servicing — i.e. the plugin dimension (`GOH_LWS_PLUGINS=ON`) loaded by a host that destroys vhosts at runtime.  The standalone daemon only destroys vhosts during context teardown (no loop service afterwards), and nothing in-tree does runtime vhost destroy today — hence Info.
- Preconditions / gating: `GOH_LWS_PLUGINS=ON` + embedding host destroying the vhost (or a future gitohashi-side runtime vhost feature).
- Attack path: vhost destroyed → PROTOCOL_DESTROY runs without cancel → up to 1s later the pt fires `dump_cb` → `lws_container_of` over freed vhd → `lws_sul_schedule(vhd->context, ...)` on freed memory → UAF, and the reschedule would keep firing.
- Evidence: `lws_sul_schedule(vhd->context, 0, &vhd->sul, dump_cb, 1 * LWS_US_PER_SEC);` (dump_cb tail, :337); PROTOCOL_DESTROY body has no sul cancel.
- Remediation sketch: `lws_sul_cancel(&vhd->sul);` at the top of PROTOCOL_DESTROY (the stat-dump body is already commented out; alternatively drop the sul entirely).

## Checked clean

Actively checked against the taxonomy, no findings:

1. **Client-controlled request intake — URL/urlarg stash** (:503-522): bounds walk verified — `end = url + 1023`, URL clamped to ≤1022 bytes; the urlarg fragment loop copies into `p+1` with size `end-p-2` (NUL included), `lws_hdr_copy_fragment` is fail-closed (-2, copies nothing) on overflow, so `p` can never pass `end-2` and the final `*p++='\0'` is in-bounds.  Overlong args are silently dropped (the client's own request becomes *less* specific; correctness note only).  Header stashes (`ua[256]`, `alang[128]`, `inm[36]`, `cc[64]`, `pr[64]`) are all-or-empty via `lws_hdr_copy`'s -1-on-overflow.
2. **ETAG flow** (:198-232): the 304 emit passes `n` (client INM total length) as the etag length — sound because a match requires `strcmp(etag, priv->inm)` equality with `inm` ≤35 chars (copy fails closed above that), forcing `n == strlen(etag)`.  `force_nocache` from Cache-Control/Pragma correctly clears INM to defeat stale 304s.
3. **Blame-shedding 503** (:587-619): priv is freed exactly once on every path (shed/enqueue-fail/NULL-vhd all call `cleanup_task_private_data` before any task ownership exists); `lws_http_transaction_completed` handled.  `strstr(priv->url, "/blame")` over-matches paths containing `/blame` as a substring (e.g. a tree path with a `blame/` dir) — cosmetic: it can only cause *more* shedding under genuine pool saturation.
4. **Threading & lifetime (steady state)**: `LWS_TP_STATUS_STOPPING` honored at the top of `task_function` (commit 3285a77) — worker returns STOPPED promptly, no pinned workers on client disconnect; `lws_threadpool_get_task_wsi` NULL-guard before `task_status` in HTTP_WRITEABLE (commit 8bc30ec); SYNC handshakes pair `task_status(SYNCING)` with `task_sync` on every non-error path, and error paths (`return -1`) leave wakeup to lws close machinery (`lws_threadpool_wsi_closing`).  `avatar()`'s lazy `vhd->cache_protocol` lookup race is idempotent (same value written); the `.user`-as-function cast is the avatar-proxy plugin's intended idiom (`mention` stored at protocol_avatar-proxy.c:505) — thread-safety of `mention()` itself is the avatar-proxy unit's recheck.
5. **Integer & buffer safety**: `priv->used` writes are bounded by the `jg2_ctx_fill` size argument (cross-unit contract, recheck filed for job-core); `(int)len` cast safe (lws caps URIs far below INT_MAX); `p - start` header writes all guarded by `lws_add_http_header_*`/`lws_finalize_*` bounds.
6. **PROTOCOL_DESTROY after failed INIT**: impossible — lws gates PROTOCOL_DESTROY on the per-protocol `protocol_init` success bit (vhost.c:1725), so the NULL `jg2_vhost`/already-destroyed-tp paths are unreachable.
7. **Info-disclosure candidates**: `lwsl_info("%s: jg2_ctx_create fail: %s", ..., start)` (:191) looks like an uninit-memory log but `start` points into `priv->buf` which is memset-zeroed at allocation (:482) — it harmlessly prints an empty string (it clearly *meant* `priv->url`; cosmetic bug, not a finding).  DROP_PROTOCOL logs the URI at info — the requester's own URL.

## Coverage

Full unit deep-read: `src/protocol_gitohashi.c` (all 794 lines).  Targeted
out-of-unit verification reads (attack-path witnesses only, not audits):
`lib/main.c` full (jg2_vhost_create/destroy, jg2_ctx_create/destroy — this
file is the lib-main unit's material and stays `never`), `lib/job/job.c`
grep-level for `ctx->vhost` deref density in the fill path,
`assets/`-none.  lws witnesses: `lib/roles/http/parsers.c`
(hdr_copy/copy_fragment), `lib/misc/threadpool/threadpool.c`
(finish/destroy/reap/cleanup_destroy), `lib/core-net/vhost.c`
(PROTOCOL_DESTROY gating), `lib/core/context.c` (context-destroy ordering).
Unit complete → status `done`.

## Methodology notes

- The `http_reply` failure-log printing an empty string instead of the
  failing URL (`start` vs `priv->url`) is a debugging-quality wart worth a
  one-line fix in some future touch of this file; not filed.
- Cross-unit rechecks filed into `coverage.md` for future passes: (1)
  lib-util — `jg2_repopath_split` handling of the still-percent-encoded URL
  stash (this unit reconstructs raw encoded urlargs into `priv->url`) and the
  repo-name `..` sanitize; (2) job-core — `jg2_ctx_fill` honoring the caller
  bound for `priv->used`; (3) avatar-proxy — `mention()` invoked from worker
  threads; (4) lib-main — control-character handling of URL-derived strings
  into lws logs (ACL-denied notice logs the repo name, main.c:675).
- No `assets/` dirs in this unit; CSP check not applicable.
