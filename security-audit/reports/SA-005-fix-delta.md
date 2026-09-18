# SA-005 — Track A fix-delta verification (13 commits, 696b960 → 2d61554) — 2026-09-13 — 2d61554

Units: `job-core` + `job-search` + `job-snapshot` + `avatar-proxy` +
`proto-gitohashi` (shed hunk) + `daemon-boot` (conf)  |  Tiers: 1+2  |  Tracks: A(delta from 696b960)
Build gating: unchanged from the audited shapes.  Working tree clean
(HEAD 2d6155448fbc9a74d1a61507c99731540d2d1e42; untracked patches/ +
this audit dir only).

## Selection

Track A: every done unit except daemon-boot's conf files gained commits —
a commissioned fix session landed fixes for all 13 then-open findings
(F-003…F-015) as one commit per finding.  The delta is not trivial, so
this pass is a fix-verification pass: each commit was diff-read and
reasoned against its finding's attack path (the lws audit's fix-delta
pattern).  All 13 verified sound; verification notes appended to each
finding's Status history, index rows marked.

## Per-fix verification

| Finding | Commit | Verdict |
|---|---|---|
| F-006 | 10f873e | `final_name[sizeof(ctx->cache)]` — accepted-domain overflow closed; no other 128-bound consumer |
| F-008 | 580bd3b | all four meta fields via `ellipsis_purify` + real PATH element; **purifier backbone independently verified**: `lws_json_purify_flags(HTML_SAFE)` escapes `"` `&` `<` `>` `=` as `\u00XX` — no quote byte survives into the double-quoted attribute, so no attribute breakout and no `http-equiv=refresh` injection (the failure mode a JSON-only `\"` escape would have left open); 4×128 scratch fits the 1024 space guard |
| F-009 | f09d2b0 | per-state `html_pos` clamps before every length computation close the unsigned wrap; nanosecond torn-window residual accepted (recorded) |
| F-012 | d46660d | local-pointer unlink/NULL/free — exact sketch |
| F-013 | ffd2b70 | extent walk keeps the owned root reference (sp guard), index walk frees all levels; refcount contract restored — the libgit2-cache grace is no longer load-bearing |
| F-014 | cdc08c6 | ac + fp loops purify into stack scratch; same verified purifier |
| F-010 | 29a5557 | both walks sliced at 128 entries with budget checked at loop top (skip-continues cannot unbound a slice), resume via partway/CHECKING_IN, `ctx->ongoing` NULL-guards added (also closes SA-004's OOM note), shed extended to search-family URLs. Residual follow-up filed (below) |
| F-011 | d2c58d3 | destroy closes+frees+NULLs `ctx->a`; error_out NULLs — no double-free |
| F-015 | 5ff0807 | `.`/`..` entries skipped before the type switch; no other traversal vector (git names cannot contain '/') |
| F-004 | e40fbf8 | `renamed` flag; nope: and DROP unlink the temp centrally and idempotently |
| F-005 | c6e587b | INIT validates remote-base (parseable + http/https); open-fail and puri-fail legs free the req soundly |
| F-003 | 07894a8 | 1MiB body cap (overflow-safe `req->got` check), non-2xx rejected at ESTABLISHED, byte budget + 1s trim sul + `lws_diskcache_prepare(0700)`, destroy cancels sul + destroys the diskcache with a NULL guard (also closing SA-002's NULL-vhd destroy note). Accepted residuals: outbound one-fetch-per-distinct-md5 stays uncapped (by design), `http://` remote-bases remain permitted (body-capped + 2xx-gated) |
| F-007 | 2d61554 | confs split the dirs with comments AND the mount is fenced to the exact `^[0-9a-f]{32}_avatar$` shape — defense in depth that also protects legacy shared-dir deployments; unsalted-key leg noted as future hardening |

## Fix-introduced follow-ups (rechecks, not findings)

1. **F-010 timeout coupling**: the build-then-answer flow ties the trie
   build to the requesting wsi's 60s `PENDING_TIMEOUT_THREADPOOL_TASK`.
   During compute-only slices the wsi sees no traffic, so an index build
   longer than the timeout may abort mid-way (trie temp litter + full
   retry on the next request).  Needs runtime confirmation; if real,
   refresh the timeout per producing slice or at slice boundaries.
2. **F-009 torn window**: clamp-then-read still spans a theoretical
   same-nanosecond reload (practically atomic size_t reads); snapshotting
   offsets under the vhost lock remains the complete fix if it ever
   matters.
3. **F-007 key hardening**: hashing a vhost identity into repo-affiliated
   cache keys would remove the cross-vhost key-equality property at the
   design level; currently fenced by mount shape + dir separation.

## Checked clean (delta-introduced code)

- `avatar_trim_sul_cb` + diskcache lifecycle: sul cancelled at destroy
  before `lws_diskcache_destroy`, `uid ? uid : (uid_t)-1` chown no-op for
  unprivileged processes is deliberate; trim walks the avatar-only dir.
- `AVATAR_MAX_BODY` accounting: `req->got + len > cap` with got ≤ cap and
  len bounded by rx buffer — no overflow; nope on breach unlinks the temp
  via the F-004 machinery.
- Search slicing: `return 0` mid-walk keeps stack/path state consistent
  for resume; `strdup("/")` re-arm at the extend→index transition matches
  the root-path free at extend-walk exhaustion; `extending`/`indexing`
  bit transitions single-direction; the removed `onetime`/outlive path
  (dead per the commit message — the stub path destroyed its own ctx) is
  not referenced elsewhere.
- Shed expansion strings match the search-family URL shapes
  (`/blame`, `/search`, `/ac/`, `/fp/`, `search=`).
- The strict avatar-mount fence (`tl != 39`, hex loop, suffix strncmp)
  matches exactly what `mention()` creates — `md5_to_hex_cstr` emits
  lowercase hex only, so no legit avatar is newly rejected.

## Coverage

All six touched units re-verified at 2d61554 (delta + surrounding
context); ledger rows bumped.  No new findings; three follow-up rechecks
filed into sweep bookkeeping.

## Methodology notes

- The delta spans 6 units against the "~2 units per pass" budget —
  treated as one delta-verification task in the lws fix-delta tradition
  (single logical change-set: the fix session); sweep work resumes next
  pass.
