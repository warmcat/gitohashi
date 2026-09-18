# gitohashi Continuous Security Audit — Coverage Ledger

Component inventory in attack-surface tiers.  Audit passes update this file:
`last-audited` / `commit` / `status` per unit.  Selection procedure and budget
rules are in `METHODOLOGY.md`.

Status values: `never` | `partial` (notes say what remains) | `done`.
A `done` unit whose paths have commits after its `commit` is a Track A
re-audit candidate, prioritized by tier.

~loc is guidance for splitting passes; big units audited across multiple
passes stay `partial` until complete.

## Tier 1 — unauthenticated client-facing surface + outbound proxy

| Unit | Scope | ~loc | Last audited | Commit | Status |
|---|---|---|---|---|---|
| proto-gitohashi | `src/protocol_gitohashi.c` (http callback: URL routing, urlargs, ETAG, bot probe, threadpool queueing, cache interaction, asset/template serving) | 800 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-001 full read: **F-001** Low shutdown-order UAF — jg2_vhost_destroy incl. git_libgit2_shutdown before threadpool joins, unauth-triggerable window via long blame + daemon restart — **fixed 0669f37** (threadpool finish+destroy join workers + reap ctxs first); **F-002** Info sul not cancelled — **fixed 696b960** (lws_sul_cancel at destroy top); clean: urlarg stash fail-closed by lws -2 semantics, ETAG n==strlen invariant, blame-shed 503 single-free, STOPPING honored + get_task_wsi guarded, PROTOCOL_DESTROY-after-failed-INIT impossible (lws protocol_init bit gate); empty-string log wart noted; SA-002 Track A verified both fix commits in committed shape at 696b960 — sound); SA-005 Track A 29a5557 shed hunk verified: search-family URLs (/search /ac/ /fp/ search=) join /blame in the 503 saturation shed — strings match the dispatch modes |
| avatar-proxy | `src/protocol_avatar-proxy.c` (outbound avatar fetch from provider, disk cache, serving) | 540 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-002 full read: **F-003** Medium unbounded cache growth — repo-pusher count leg + provider/MITM uncapped-body leg, no status check before rename, no budget/LRU, outbound amplification; **F-004** Low nope-path unlinks final not temp + DROP never unlinks → temp leak per failed fetch; **F-005** Low puri-fail early-return orphans req (fd+temp+memory); clean: server-side traversal fence (single-element + '..' reject, no %-decode, query stripped), mention() worker-thread safety CLOSED (dll2 ops under recursive vhd->lock, remove-under-lock before free, daemon table order joins workers before avatar mutex destroy — the SA-001 recheck), happy-path fd/req lifecycle + wsi-user NULL fencing, buffer chain 128/192/256 consistent; notes: shutdown-window destroyed-mutex lock by late DROP (F-001 family, milder), plugin-embedding protocol-order fragility, 8×1s parked connections on nonexistent names, provider error pages cached as avatars → F-003 remediation); SA-005 Track A F-003/F-004/F-005/F-007 fixes verified (07894a8 body cap + 2xx gate + budget/trim sul + destroy cleanup incl. the SA-002 NULL-vhd note; e40fbf8 temp unlinks via renamed flag; c6e587b INIT remote-base validation + orphan cleanups; 2d61554 mount fenced to ^[0-9a-f]{32}_avatar$); residuals: outbound fetch rate uncapped by design, http:// remote-base still permitted (capped + 2xx-gated) |
| daemon-boot | `src/main.c` + `etc-gitohashi/` JSON conf + `system/` (context/vhost creation, CSP + TLS posture of shipped defaults) | 150+conf | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-003: **F-007** Medium shared cache dir — avatar mount serves raw JSON cache entries cross-vhost, unsalted vhost-independent repo-job keys make private-on-B/public-on-A views computable + fetchable via `/git/avatar/<name>`; clean: shipped CSP strict (no unsafe-inline anywhere, frame-ancestors/base-uri none), uid/gid drop via conf, VALIDATE_UTF8 + EXPLICIT_VHOSTS, JSON config parsing is lws-side; posture notes: SIGHUP unhandled so systemd reload = restart, killall ExecStop, non-volatile-sig_atomic signal flag, no systemd hardening directives; gitohashi-selinux.pp binary not audited); SA-005 Track A 2d61554 conf hunks verified: avatar cache-dir split from json cache-base in both example vhosts with explanatory comments |

## Tier 2 — job pipeline over repo content (threadpool + libgit2)

Repo content (commit messages, author identities, file/ref names, blobs) is
attacker-influenced by anyone with push access to a displayed repo.

| Unit | Scope | ~loc | Last audited | Commit | Status |
|---|---|---|---|---|---|
| job-core | `lib/job/job.c` + `lib/job/private.h` (threadpool job lifecycle, service thread, wsi interplay, result streaming) | 1250 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-003 full job.c read: **F-006** Medium final_name[128] stack overflow on cache finalize for accepted 92–256-char cache-bases (ctx->cache[384] sized right, final_name missed); **F-008** Low raw reponame/mode/vid into HTML meta content attr (mode/vid free-form; CSP-fenced; doubled-NAME copy-paste bug in same line); **F-009** Low template hot-reload race — unlocked worker reads of vh->meta/dynamic/html_len + stale html_pos → size_t wrap → memcpy OOB read into responses; clean: **lws_snprintf clamp verified** (returns ≤ size, 0 on size 0 — CTX_BUF_APPEND can never pass end, cursor-overshoot class absent file-wide), spool 6-byte tail-window arithmetic exact-boundary safe incl. empty/exact-multiple reads + ]}-strip fixup in-bounds, cache_write partial-fail close+unlink+degrade, job table internal-indexed, chained-job flags consistent, error path purified; **SA-001 recheck CLOSED** — jg2_ctx_fill honors the caller bound by construction (end = buf+len-1 + the clamp)); SA-005 Track A F-006/F-008/F-009 fixes verified (10f873e final_name sized to ctx->cache; 580bd3b meta fields purified — HTML_SAFE \u00XX escaping independently verified, no attribute/meta-refresh breakout; f09d2b0 html_pos clamps); residual torn-window + F-010 timeout rechecks filed |
| job-search | `lib/job/search.c` | 890 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-004 full read: **F-010** Medium unyielded full-repo indexing in one task slice — unauth worker-pool exhaustion, shed doesn't cover search; **F-012** Low remove_ongoing free(NULL) leak per index build; **F-013** Low freed root-tree pointer restashed/reused across extent→index walks (libgit2 cache ref is the only thing holding it); **F-014** Low unpurified ac/fp trie strings into JSON (repo filenames + content tokens — JSON injection, CSP-fenced, F-008 class); clean: ongoing/CREATING cache dance (close/unlink only ever hits fresh temps or no-ops), strcpy hash exact-fit hash[33], trie_filepath[512] fits, finalize_name truncates string so immediate reopen uses final name, indexing_list under recursive vhost lock, check_indexed callable from meta_trailer; notes: OOM-path ctx->ongoing NULL deref (:513/:777), whitelist[n] priority scramble (functional), failed-index trie temp leak (trim self-heals), depth>16 fails); SA-005 Track A F-010/F-012/F-013/F-014 fixes verified (29a5557 128-entry sliced walks + build-then-answer + ongoing NULL guards; d46660d ongoing free fixed; ffd2b70 root-tree ownership restored; cdc08c6 ac/fp purified) — follow-up: 60s wsi-timeout coupling of huge builds needs runtime confirmation |
| job-snapshot | `lib/job/snapshot.c` + `lib/job/no-snapshot.c` (libarchive tar/zip generation, `JG2_HAVE_ARCHIVE_H` gating) | 620 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-004 full read: **F-011** Medium aborted-snapshot libarchive-writer leak (destroy path never frees ctx->a — unauth repeatable, memory exhaustion); **F-015** Low tar-slip member names from crafted `..` tree entries (plumbing push, no fsck; extractor-dependent impact); clean: a_write/LAC replay arithmetic consistent + bounded, blob streaming partial-write resume + exact free, snapshot URL parsing bounded (hex_oid[64], pure[4096]), ref resolution fail-closed, symlink/submodule entries bail (safe-by-refusal), no-snapshot stub fail-closed; note: error_out should NULL ctx->a after free when F-011 is fixed); SA-005 Track A F-011/F-015 fixes verified (d2c58d3 archive writer freed on destroy, error_out NULLs; 5ff0807 . / .. entries skipped) |
| job-blame-commit | `lib/job/blame.c` + `lib/job/commit.c` | 920 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-006 full read: **F-016** Medium whole-diff lwsac materialization in one start slice — pusher-planted huge-text diff, unauth-detonatable, daemon-wide memory pressure; clean: common_print_cb exact allocs, lac-walk ofs/size/pos arithmetic incl. lwsac_sizeof(1)-vs-(0) first-chunk header (verified against lwsac.c), purify discipline on every repo-content emission (raw only in text/plain plain/patch modes), goto-chain frees exactly once, blame lwsac serialization before git_blame_free; notes: same-sig list insertion no-op (dead list), error-path orig-commit leak, dangling-but-unused copied hunk signature pointers, commit/patch URLs not in the shed set) |
| job-tree-plain | `lib/job/tree.c` + `lib/job/plain.c` (tree browsing, raw file serving) | 590 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-006 full read: tree listings accumulate the F-016 lwsac class (per-entry); clean: purified emissions everywhere (bloblink composition incl. the fenced trailing-slash strcat), blob streaming m/6 purify cap + reserve seals, plain memcpy bounded + mmap-backed (memory-cheap), inline_filename composition bounded + git-tree bypath cannot traverse, blob-path frees exactly once; notes: strrchr-from-last-char basename extraction broken (blobname shows full path — cosmetic), container-of-NULL head compute after blob destroy (unused), README-inline fallback extensions for blog mode) |
| job-blog | `lib/job/blog.c` (blog mode, `YYYY/MM/DD` repo traversal, article scraping) | 400 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-007 full read: **F-017** Low 2500-byte item reserve vs ~3644 worst-case item — clamped mid-JSON truncation written to cache → persistent blog-index breakage until refs change; treewalk lwsac accumulation folded into F-016 as a variant; clean: scraping state machine bounds verified (line-walk memchr, byte-guarded summary appends, bracket/paren searches length-bounded; close_bracket[1] reads the line terminator, in-bounds of the odb trailing-NUL buffer), all four emitted strings purified, lwsac record layout exact + oid_copy stable, digit-gated YYYY/MM/DD descent skips all other trees; notes: raw path in lwsl_notice lines (log-injection class → lib-main recheck), manual ctx->partway set redundant) |
| job-lists | `lib/job/log.c` + `lib/job/repos.c` + `lib/job/reflist.c` | 370 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-007 full read: **F-018** Low/high uninitialized `git_commit *c` deref in the log end-block when count exhausts exactly at the root (chain == count+1) — ENOTFOUND leaves c stale (in practice the previous iteration freed commit), garbage "next" oid or crash; clean: log loop-body c=NULL advance safe, 768 reserve covers commit_summary ≤~330 (witness-verified in util.c), repos ACL under vhost lock + sized 100+size guard + purified emissions + identity_json owner, reflist iterator freed on all paths + dangling-symbolic-ref continue (the documented rref fix) + purified ref names + generic_object_summary {} degradation; **TIER 2 COMPLETE**) |

## Tier 3 — library core, conf parsing, identity

| Unit | Scope | ~loc | Last audited | Commit | Status |
|---|---|---|---|---|---|
| lib-util | `lib/util.c` (JSON purify / ellipsis helpers, repo-name sanitize, identity string composition — the escaping heart of the injection chain) | 1270 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-008 full read: **F-019** Low duplicate-param strdup leak in the urlarg loop; clean: **SA-001 repopath recheck CLOSED** (no %-decoding — encoded traversal inert; .. rejected any-position in NAME+PATH; functional false-positive noted: no-slash URLs scan urlargs for ..), F-008 sr.e[] shape recheck CLOSED, SA-006/007 util recheck CLOSED (blob_from_commit bounded + fail-closed, signature/identity/name_email all purified, ellipsis_text fences text-mode control chars, commit_summary ≤~330 matches the 768 reserves), purify family max-3 headroom verified exact, RFC2047 decoder strictly validated (charset allowlist, b64/Q gates, tmp/2 raw cap vs latin1 2x in-place, decoded-once, canonical UTF-8 validator), jg2_json_oid alias cap 8 purified under jrepo lock; notes: param-missing-= returns success, repopath VIRT_ID 64-byte truncation functional) |
| lib-main | `lib/main.c` + `lib/private.h` + `include/libjsongit2.h` (context/vhost creation, destroy, path handling, public api) | 1710 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-009: main.c re-verified from SA-001 + private.h full struct/prototype read (sizes confirm all prior buffer arithmetic: cache[384], trie_filepath[512], stack[16], last_from_cache[6], hex_oid[64]) + libjsongit2.h walked (no new input paths); **rechecks CLOSED**: SA-001 log-injection (URL-derived strings fenced by the lws URI C0 fence on current lws — version caveat recorded for older system lws), F-006 fence side (JG2_CACHE_BASE_MAX + ctx->cache sizing complete with 10f873e), F-009 reload side (mtime reload under vh lock + worker clamps; torn-window residual stands); empty-string log wart from SA-001 remains cosmetic) |
| lib-cache | `lib/cache.c` + `lib/repostate.c` (transparent JSON cache, ref-hash invalidation) | 440 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-008 repostate full read + SA-003 cache re-check: **F-007 key-equality recheck (cache side) CLOSED** (no vhost-specific key material; suffix namespaces object kinds only); clean: __repo_reflist_update hash-chain snip/prev invariant holds **given libgit2 deterministic name-ordered ref iteration** (constraint recorded), symbolic-ref skip in both loops (documented SIGSEGV fix), coherence-break tail frees exact, md5 recompute serialized under vhost+jrepo locks, 3s rewalk throttle, refchange cb = no-op logger safe from trim thread, cache.c vsnprintf clamp + bounded suffix compose; notes: OOM-path iterator leak in regenerate loop, huge-ref-count 3s walks = F-016-adjacent amplification) |
| conf-gitolite | `lib/conf/gitolite/gitolite3.c` + `common.c` + `lib/conf/scan-repos.c` + `lib/conf/private.h` (gitolite ACL parsing — the private-repo authz boundary — + repo discovery) | 1180 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-009 full read: **F-020** Low rei lwsac freed under rd->lock on gitolite-admin change races vh-lock cache-hash walks + unlocked repos-start pickup (UAF read, stale cache keys); clean: auth-name charset gate (argv + cache-key injection fenced, invalid→NULL deny-path, @all = operator pvo semantics), privilege-dropped gl3 child (setgroups-first fatal drop), mkstemp+unlink+O_NOFOLLOW subprocess protocol (symlink races fenced, documented), query mutex serialization, umask dance, chunked ACL parser (pos fences, refs/.*-only admission, unknown repos skipped), scan-repos measure-then-fill exact + gitolite-admin hidden; notes: stderr dump of ACL results (:646 debug leftover), unchecked stdin-list writes, raw waitpid status logged) |
| email | `lib/email/email.c` + `md5.c` (commit author email parsing → gravatar md5) | 535 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-010 full read: 0 findings; **SA-002 recheck CLOSED** — bins LRU bounded (16×16×64B) w/ correct move-to-front + documented depth-1 head fix, JG2_EMAIL_MAX_LEN caps hash+md5 work, avatar cb under vh lock w/ one-way lock order, OOM → "?" downstream; md5.c = vendored mbedtls fixed-struct impl (non-crypto use); notes: >63-char emails never cache-hit (correct, uncached), nonpositive email_hash_bins from library consumers would mis-index (no in-tree setter)) |

## Tier 4 — client-side assets, templates, exemplars

| Unit | Scope | ~loc | Last audited | Commit | Status |
|---|---|---|---|---|---|
| assets-jg2 | `assets/jg2.js` (clientside JSON→DOM renderer, innerHTML sinks, showdown/highlight interplay) | 2680 | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-010 full read: **F-021** Low san omits ' while tree hrefs/identity alt/aliases tgt are single-quoted w/ repo content (filenames, refnames, emails legally contain '), + breadcrumb e[n]/makeurl hrefs interpolate raw rpath/qbranch/qid from window.location — CSP-fenced markup injection, pusher + unauth legs; **F-014 DOM-sink recheck CLOSED** — every JSON-derived string at the sinks passes san/san_nq/identity (search tokens san'd, blog fields san'd, blobs san_nq'd before showdown reads textContent); clean: purify double-fence (server \u00XX + client san), showdown/hljs preprocessing contract honored (san_nq zero-width-space, makeHtml from textContent), identity URLs = operator pvo + hex md5, aging/menu/copy numeric-or-constant; notes: vendored i18n lobalContext typo, doc_dir global) |
| assets-vendored | `assets/showdown.min.js` + `highlight.pack.js` + css + `templates/gitohashi-example.html` (vendored version/CSP glance + preprocessing-contract interaction only; full vendored-lib audit out of scope) | — | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-011 glance: **F-022** Low vendored staleness — showdown 2.0.0-alpha1 (2018) + hljs 9.18.1, years of downstream security fixes missed; hljs grammar ReDoS = viewer-tab hang on crafted code files, showdown leg CSP+preprocessing-fenced; clean: template markers exactly-once, all script/style/font refs same-origin, no inline script/style, JSON-div injection backed by the verified HTML_SAFE purification, css external-ref-free (same-origin font only), preprocessing contract unchanged (SA-010)) |
| examples-corpus | `examples/minimal/jg2-example.c` + `examples/threadchurn/threadchurn.c` + `xss/` corpus (public-api usage exemplars; corpus coverage vs the preprocessors) | small | 2026-09-13 | 2d6155448fbc9a74d1a61507c99731540d2d1e42 | **done** (SA-011 full read: 0 findings — jg2-example correct api pairing + local-trusted input; threadchurn bounded 16×10k churn of the multithreaded paths (the F-020 class); xss corpus = OWASP vectors w/ no-alert-box criterion, covers the showdown leg (NOT the jg2.js repobar path where F-021 lives — re-run after any escaper/vendored bump); note: pthread_join over all slots on create-failure) |

## Sweep bookkeeping

- **Audit initialized 2026-09-06.** The audit tracks the **checked-out
  branch**: at init that is `threadpool-lockup-fixes` @
  `65d7e7a839f1d332f9be3b8bd9f0a0a59b7206e3` (canonical main branch:
  `master`).  If the working tree moves to another line, record the
  realignment here (as the lws audit does) — all previously recorded hashes
  stay resolvable via reflog/other branches.
- Untracked `patches-*/` dirs at the repo root are stgit series storage for
  downstream fix work, not build inputs; ignore them for coverage.  If a
  patches dir starts mirroring source files that differ from the tree, treat
  those diffs as uncommitted WIP for stamping purposes.
- Current-branch commits already carrying security-relevant work (context
  for Track A, not findings): `3880bc3` audit-fixes (CID-family fixes),
  `3285a77`/`02df748`/`8bc30ec` threadpool STOPPING / 503 shedding /
  get_task_wsi NULL-guard (the taxonomy-4 fix family), `65d7e7a`
  json-purify-improvements (the taxonomy-2 escaping core, incl. jg2.js).
  First sweep reads these regions in their current shape regardless.
- Next free report seq: `SA-016` (see `reports/`)
- Next free finding id: `F-024` (see `findings.md`)
- **FIRST FULL SWEEP COMPLETE (SA-011, 2026-09-13)** — every unit in
  all 4 tiers has a status.  Totals: 22 findings filed, 15 fixed
  (SA-005-verified), 7 open (F-016..F-022).  The audit now runs
  Track A only until new commits land; open findings await triage
  / a commissioned fix session.
- **Fix session 2026-09-13** (696b960 → 2d61554): all 13 open findings
  F-003…F-015 fixed one-commit-per-finding; **SA-005 verified all of
  them in committed shape** (per-finding notes in Status histories).
  Rechecks filed:
  - F-010 follow-up — RESOLVED by SA-012 (code-level): the 60s timeout
    is a one-shot sul armed once and never re-armed — build-completion
    regression confirmed + the same cap truncates all >60s threadpool
    responses; filed as F-023 with the re-arm remediation.
  - F-009 residual: clamp-then-read torn window (theoretical, accepted);
    offset snapshot under lock stays on the shelf.
  - F-007 hardening shelf: vhost identity into repo-affiliated cache keys
    (exposure currently fenced by mount shape + dir separation).
- Cross-unit rechecks for future passes:
  - `lib-util` pass additions (SA-006): verify `blob_from_commit`'s
    path->blob resolution and `signature_json`/`identity_json`/
    `signature_text`/`commit_summary` escaping when that unit is
    deep-read (witnessed purified at call sites; the implementations
    are lib-util material).
  - `lib-util` pass: `jg2_repopath_split` must handle the still-percent-
    encoded URL that proto-gitohashi stashes (raw URI + raw encoded urlargs
    reconstructed with `?`/`&`), and the repo-name `..` sanitize
    (`lib/util.c:83`) must be checked against that input shape (SA-001);
    also confirm the decoding/sanitize shape of mode/vid/path reaching
    `ctx->sr.e[]` for F-008's injection surface (SA-003).
  - `lib-main` pass: log-injection recheck — URL-derived repo name reaches
    `lwsl_notice` at ACL-denied (main.c:675) and the URI at
    HTTP_DROP_PROTOCOL info (protocol_gitohashi.c:642); what do control
    chars in the URL do to lws log emission? (SA-001).  Rides along:
    F-006's `JG2_CACHE_BASE_MAX` fence + `ctx->cache` sizing side, and
    F-009's reload side (`__jg2_vhost_reference_html` reload cadence +
    lwsac lifetime under swap).
  - `lib-cache` + `conf-gitolite` passes: F-007's key-equality leg —
    confirm no vhost identity enters repo-affiliated cache keys once those
    units are deep-read (SA-003).
  - `email` pass: the avatar cb input contract was witness-verified in
    SA-002 (md5 always computed before the cb — 32-hex, NULL only renders
    as inert "?"); check the email bins cache bounding (recycling seen at
    witness level) and JG2_EMAIL_MAX_LEN enforcement when deep-reading
    email.c (SA-002).
  - `assets-jg2` pass: F-014's injected-JSON members are consumed at the
    jg2.js DOM sinks — verify what unpurified members can reach there
    (SA-004).
  - Closed by SA-003: SA-001's `jg2_ctx_fill` caller-bound recheck
    (the lws_snprintf clamp + `end = buf+len-1` make it hold by
    construction); F-001's fill-path `ctx->vhost` deref recheck
    (confirmed in fixed destroy order, SA-002 + SA-003).
