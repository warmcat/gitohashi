# SA-004 — job-search + job-snapshot — 2026-09-06 — 696b960

Units: `lib/job/search.c` (892 loc); `lib/job/snapshot.c` + `no-snapshot.c` (593+28 loc)  |  Tier: 2  |  Tracks: B(sweep) ×2
Track A check: empty (HEAD unchanged at 696b960 since SA-003).  Build
gating: search is default (lws fts); snapshot requires `JG2_HAVE_ARCHIVE_H`
(libarchive found — the common build; otherwise the no-snapshot stub
fail-closes by returning -1, verified).  Working tree clean (HEAD
696b960dcedb3a05749ad3542044228c7329e780).

## Build/entry-point framing

Both are jg2 jobs: entered via `jg2_ctx_fill` → `ctx->job(ctx)` on
threadpool workers, fed by URL path elements (`ctx->sr.e[]`) and repo
content via libgit2; search additionally touches the JSON cache dir (trie
index files, `<hash>` names without suffix — part of the F-007 surface)
and the `indexing_list` on the shared jrepo; snapshot streams libarchive
output through the ctx buffer + lwsac side-chains.  `no-snapshot.c`
stub-fails closed.

Verified supporting contracts: `lws_snprintf` clamp (SA-003), libgit2
`git_commit_tree` returns an owned reference, libgit2's object cache is
enabled by default (gitohashi never disables it — main.c's commented opts
line only mentions owner validation) which is what keeps F-013 benign in
practice.

## Findings

### F-010 [Medium/medium] Full-repo search indexing runs unyielded in a single task_function slice — unauthenticated clients can pin all threadpool workers for the entire index build, and the blame-shed does not cover indexing
- Location: `lib/job/search.c:405-507` (extent walk), `:683-816` (index walk — both `do {…} while(1)` with no buffer/yield checks), workers only observe `LWS_TP_STATUS_STOPPING` at task_function entry (protocol_gitohashi.c:95)
- Reachability: unauthenticated `GET /git/<repo>/search/…` (or any view that chains the search job) on a repo whose current refs state has no cached trie → worker builds the full index (two complete tree walks + full-text index of every whitelisted file) before returning.
- Preconditions / gating: default build; impact scales with hosted repo size (kernel-scale repos → minutes per index).
- Attack path: attacker requests search on the largest unindexed repos → 4 workers each enter an uninterruptible index build → the queue (depth 12) fills with any subsequent requests → every gitohashi view stalls; the 503 shedding (02df748) only triggers for URLs containing `/blame`.  Refs changes (pushes) re-trigger new tries; the `ongoing` dedup only collapses concurrent identical index builds.
- Evidence: no `JG2_HAS_SPACE`/partway logic inside either walk loop (contrast snapshot.c which yields on buffer state); `ctx->outlive` decouples from the wsi but not from the worker.
- Impact assessment: unauth-triggerable whole-service stall gated on large hosted repos — first-touch forcing of every hosted repo's index is an availability lever.  Medium, medium confidence (mechanism code-verified; duration depends on repo sizes).
- Remediation sketch: yield inside the index walk every N entries (set partway, return 0 like snapshot's blob streaming), and/or include search-mode URLs in the shed heuristic when the pool is saturated.

### F-011 [Medium/medium] Aborted snapshots leak the libarchive writer: `job_snapshot_destroy` never frees `ctx->a` — unauthenticated, repeatable memory leak per truncated download
- Location: `lib/job/snapshot.c:316-340` (destroy omits `archive_write_free(ctx->a)`), the destroying entry at `:350-354`, ctx teardown path `lib/main.c:354-359` (`ctx->job(ctx)` re-invocation on destroy)
- Reachability: unauthenticated client starts `GET /git/<repo>/snapshot/<repo>-<ref>.tar.gz` on any repo, disconnects mid-transfer → wsi closes → task stopped → ctx destroy → job_snapshot(destroying=1) → `job_snapshot_destroy` frees trees/blob/lwsac but not the archive writer (gzip/bzip2/xz filter state included).
- Preconditions / gating: default build with libarchive (`JG2_HAVE_ARCHIVE_H`).
- Attack path: repeat request+disconnect N times → N libarchive writers + compressor states leak (~100s of KB each) → daemon memory exhaustion (the shipped systemd unit caps LimitAS=1500M → OOM kill; Restart=on-failure recovers, so this is degradation/DoS, not persistence).
- Evidence: `error_out` (:586-587) and the completion step (:574-575) both free `ctx->a` — only the destroy path misses it; `ctx->a` is non-NULL from `job_snapshot_start` success until those two exits.
- Impact assessment: unauth, unbounded-rate resource leak — practical memory-exhaustion DoS on the default deployment; Medium, medium-high confidence (leak path code-verified; not runtime-reproduced this pass).
- Remediation sketch: `if (ctx->a) { archive_write_free(ctx->a); ctx->a = NULL; }` in `job_snapshot_destroy` (idempotent with the two existing free sites if they NULL it, which completion does and error_out should).

### F-012 [Low/high] `remove_ongoing` nulls the pointer before freeing — `free(NULL)` no-op leaks the `ongoing_index` on every completed or aborted index build
- Location: `lib/job/search.c:47-59` — `ctx->ongoing = NULL; free(ctx->ongoing);`
- Reachability: every trie index completion (`:825`) and every search-ctx destroy; unauth-triggerable via search requests (bounded by distinct (repo, refs-state) trie builds).
- Attack path: list unlink succeeds → `ctx->ongoing = NULL` → `free(ctx->ongoing)` frees nothing → ~48-byte struct leaks per index build.  Pusher-driven refs churn multiplies it.
- Evidence: the three-line sequence; intent obvious (`free` was meant to run on the unlinked node).
- Impact: slow memory leak; Low, high confidence.
- Remediation sketch: keep a local `struct ongoing_index *o = ctx->ongoing;`, unlink, `ctx->ongoing = NULL`, `free(o)`.

### F-013 [Low/medium] `job_search_start` frees the root tree in the extent walk, then re-stores and reuses the freed `u.tree` pointer in the index walk — correctness only survives via libgit2's object-cache reference
- Location: `lib/job/search.c:413-422` (extent-walk level exhaustion frees every tree including the root at sp==0), `:526` (`ctx->stack[0].tree = u.tree;` — the same pointer just freed), `:690` (first deref in the index walk)
- Reachability: every trie index build (the normal path, unauth-triggerable).
- Preconditions / gating: works today because `git_commit_tree`'s owned ref is dropped by the extent walk while libgit2's default-on object cache holds a second reference; the object is only actually freed if the cache evicts it in the straight-line window between the two walks — possible under concurrent libgit2 load on the other workers / cache-trim thread driving cache pressure.
- Attack path: large repo indexing (loads many objects → cache pressure) while other workers run heavy jobs → eviction drops the cache ref on the tree with refcount 0 → `git_tree_entry_byindex` on freed memory → UAF read/crash in the worker.
- Evidence: the extent walk's `git_tree_free(lev->tree)` at root exhaustion vs the unconditional re-store at :526; snapshot.c does not share the shape (single walk, no reuse).
- Impact: refcount-contract violation with a rare eviction race on top; Low, medium confidence (violation is code-verified; the eviction race is reasoned, not reproduced).
- Remediation sketch: re-lookup the tree by oid for the second walk, or don't free stack[0].tree at root exhaustion of the *extent* walk (only the index walk owns teardown).

### F-014 [Low/high] Search/autocomplete/filepath results interpolated into JSON without purification — repo-controlled filenames and file-content tokens inject JSON structure (CSP-fenced at render)
- Location: `lib/job/search.c:611-627` (ac loop — `((char *)(ctx->ac + 1))` tokens from indexed file contents), `:647-671` (fp loop — `((char *)(ctx->fp + 1)) + matches_length` file paths), both raw `%s` into `CTX_BUF_APPEND`
- Reachability: repo pusher commits whitelisted files (`*.c`, `README`, …) whose *names* or *contents* contain `"` / backslash / control chars → indexed into the trie → served raw into the JSON (which is embedded in the HTML sandwich for search views).
- Preconditions / gating: default build; render-time impact fenced by the strict CSP (script-src 'self') to markup/JSON-structure injection — same class and grading as F-008.
- Attack path: crafted filename `a", "evil": "` (git allows quotes in names) → trie → fp result → `{"fp": "a", "evil": "", …}` — injected members consumed by jg2.js; autocomplete tokens from file bodies give the same via content.
- Evidence: every other repo-derived emission in the job family goes through `ellipsis_purify`/`jg2_json_purify`; these two loops are the gap.
- Impact: JSON structure + (CSP-fenced) markup injection from repo content; Low, high confidence the interpolation is unpurified.
- Remediation sketch: purify both strings (`ellipsis_purify` into a scratch, as meta_header does) before the `%s`.

### F-015 [Low/medium] Snapshot archive member names taken verbatim from git tree entries — crafted `..` directory entries produce path-traversal archive members (tar-slip on extraction)
- Location: `lib/job/snapshot.c:498-499, 539-542` (`path` = walk prefix + `git_tree_entry_name(te)` → `archive_entry_set_pathname`)
- Reachability: repo pusher crafts tree objects (plumbing; git clients refuse to *create* `..` names but the server accepts crafted objects absent `receive.fsckObjects`) with directory entries named `..` → nested levels yield members like `<prefix>/../../evil` in the served tarball/zip.
- Preconditions / gating: default build + crafted-object push; the final impact depends on the *extracting* tool (GNU/bsdtar refuse or strip `..` members; Python tarfile and older tools extract literally).
- Attack path: crafted nested `..` tree entries → snapshot request → archive contains escaping member paths → a user extracting with a literal-extraction tool writes outside the target dir.
- Evidence: no sanitization of entry names anywhere in the walk; git forbids '/' in tree entry names, so traversal requires the crafted-`..` route (multi-level escape via nested crafted trees is constructible).
- Impact: downstream extraction hazard rooted in gitohashi serving unvalidated names; Low, medium confidence.
- Remediation sketch: reject/skip tree entries whose name is `.` or `..` during the walk (cheap and closes the crafted-object route entirely).

## Checked clean

1. **a_write / LAC replay machinery** (:51-110, :361-407): direct-copy bounded by `end - p`; lwsac chunks exact-sized with 4-byte length prefixes; replay advance/consume arithmetic consistent (`lwsac_sizeof(0)+4` payload offsets, `chunck_end` from the stored prefix); replay loop gated on `p != end`; libarchive's "claim full len" contract respected.
2. **Snapshot URL parsing** (:137-314): rev extraction bounded into `hex_oid[64]`; format suffixes fixed strings; `pure` prefix bounded (`n ≤ sizeof-2`); ref resolution branch→tag→oid fail-closed; `strrchr('-')` unused-result is just the existence check (coverity comment).
3. **Blob streaming** (:430-449): partial writes resume from `ctx->pos`, blob freed exactly on completion; symlink/commit/submodule tree entries hit the `default:` bail (snapshot fails closed on exotic trees — safe-by-refusal; functional limitation).
4. **search start/ongoing dance** (:184-319): `close(fd_cache)`/`unlink(ctx->cache)` only ever hits a just-created temp (job-spool would have consumed an EXISTS hit) or an already-closed -1 / nonexistent path; `strcpy(ongoing->hash, hex)` exact-fit into `hash[33]`; `trie_filepath[512]` fits the diskcache path; `lws_diskcache_finalize_name` truncates the passed string so the immediate `lws_fts_open` uses the final name.
5. **Threading**: `indexing_list` mutations under the vhost (recursive) lock on both sides; `check_indexed` callable from meta_trailer contexts.
6. **no-snapshot stub** fail-closed (-1 → error JSON), so the no-libarchive dimension never serves partial archives.

Notes (not findings): the extent-walk `ctx->ongoing->index_files_to_do` read at :513 and bump at :777 NULL-deref if the earlier `malloc(ongoing)` failed (OOM path crashes rather than degrading — robustness); `whitelist[n].priority` at :796 indexes the array by the matched *suffix length*, not the entry index — search-result priorities are scrambled (functional); failed index builds leave the trie `~temp` file behind (no unlink on bail) — partially self-healing since diskcache trim counts all regular files (F-003 nuance); tree depth >16 fails indexing (bounded, functional).

## Coverage

Full deep-read: `lib/job/search.c` (all 892 lines), `lib/job/snapshot.c`
(all 593), `lib/job/no-snapshot.c` (28); private.h struct fields verified
(`ongoing_index.hash[33]`, `trie_filepath[512]`, `stack[16]`).
Witness reads: lws diskcache finalize semantics (SA-003), libgit2
`git_commit_tree` ownership contract.  Both units → `done`.

## Methodology notes

- None.
