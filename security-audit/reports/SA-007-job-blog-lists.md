# SA-007 — job-blog + job-lists — 2026-09-13 — 2d61554

Units: `lib/job/blog.c` (399 loc); `lib/job/log.c` (128) + `repos.c` (129) + `reflist.c` (109)  |  Tier: 2  |  Tracks: B(sweep) ×2
Track A check: empty (HEAD = ledger commit 2d61554).  Build gating:
blog needs blog mode (vhost flag + blog-repo-name, or a repo description
starting '+'); lists are default.  Working tree clean (HEAD
2d6155448fbc9a74d1a61507c99731540d2d1e42).

## Build/entry-point framing

blog: vhost-faked repo/mode (`/vpath[/path]`), collects every
`YYYY/MM/DD/*.md|mkd` blob across the whole tree into a reverse-sorted
lwsac list, then emits per-post title/summary/image scraped from the
first 4KiB of each blob.  lists: log (first-parent walk, count-limited),
repos (repodir listing + ACL), reflist (reference iterator + peeled
targets).  All worker-thread jobs fed by repo content.

Witness check for the guards: `commit_summary`/`generic_object_summary`
emit ≤ ~330 bytes worst case (100-byte purified summary + oids +
signature_json) — the 768-byte reserves in log/reflist are correctly
sized (unlike blog's, below).

## Findings

### F-017 [Low/medium] Blog item space check reserves 2500 bytes but an item can emit ~3644 — mid-JSON truncation that lands in the cache, persistently breaking the blog index until refs change
- Location: `lib/job/blog.c:236` (`JG2_HAS_SPACE(ctx, 2500)`) vs the item appends at :378-384 (pure_path 512 + pure_title 512 + pure_summary 2048 + pure_image 512 + overhead ≈ 3644 max)
- Reachability: repo pusher posts an article whose scraped first-paragraph summary purifies near the 2048 scratch cap (long first paragraph — ordinary blog content can do it), preceded in the same buffer by an item that leaves between 2700 and 3644 bytes free.
- Preconditions / gating: blog mode (opt-in flag), default build.
- Attack path: item N is small (buffer left in [2700, 3644)) → check passes → item N+1's appends hit the `CTX_BUF_APPEND` clamp mid-string → the item's JSON is truncated without its closing quote/brace → the whole page's JSON is unparseable by jg2.js **and the malformed bytes are cache-written** → every subsequent viewer gets the broken page until the blog repo's refs change.
- Evidence: `JG2_HAS_SPACE(ctx, 2500)` vs the four scratch buffers' summed max; the clamp (lws_snprintf) makes it truncation rather than overflow, so this is malformed-output, not memory unsafety.
- Impact assessment: persistent page breakage for the blog view (pusher-gated, unauth-viewed).  Low, medium confidence (arithmetic verified; the exact trigger needs the buffer-leftover window, which normal pagination makes routine).
- Remediation sketch: compute the item's actual size (sum of the purified lengths + overhead) and break/resume when it does not fit — the pattern log/tree use with sized guards — or raise the reserve to cover the worst case (≥ 3700).

### F-018 [Low/high] `job_log` end-block dereferences an uninitialized `git_commit *c` when the count exhausts exactly at the root commit — garbage/freed pointer fed to `git_commit_id`, emitting a bogus "next" oid or crashing
- Location: `lib/job/log.c:83` (`git_commit *c;` — uninitialized), `:89` (`git_commit_parent(&c, ctx->u.commit, 0)` — leaves `c` untouched on GIT_ENOTFOUND), `:90-93` (`if (c)` → `jg2_json_oid(git_commit_id(c), ...)`)
- Reachability: deterministic per repo shape — when the repo's first-parent chain length is exactly count+1 (51 for log views, 11 for summary's chained log), the count exhausts while `u.commit` is the root commit; the parent lookup fails without writing `c`.
- Preconditions / gating: none beyond defaults; unauthenticated log/summary view of a qualifying repo.
- Attack path: root commit has no parent → `git_commit_parent` returns GIT_ENOTFOUND without touching `*out` → `c` holds stale stack contents (in practice the previous loop iteration's freed commit pointer — the same stack slot) → `if (c)` takes the branch → `git_commit_id(c)` reads the freed/garbage commit → a garbage "next" oid is emitted into the JSON (clicking it 404s), or the worker crashes on an unmapped pointer.  In practice the libgit2 object cache usually still holds the freed commit, masking it.
- Evidence: the asymmetry inside the same file — the loop body at :118 does `c = NULL;` before the identical call (and reflist.c:75-85 documents the same class) — the end-block simply predates the discipline.
- Impact assessment: uninitialized-pointer deref (read) with attacker-influenced timing via repo shape; typical outcome a corrupt "next" pointer in the JSON, worst case a worker crash.  Low, high confidence (mechanism code-verified against libgit2's ENOTFOUND contract).
- Remediation sketch: `git_commit *c = NULL;` at :83 — and gate the "next" emission on `!git_commit_parent(&c, ...)` succeeding.

## F-016 variant note

`treewalk_cb_blog` (blog.c:63-135) joins F-016's variant list: one
`tree_entry_info` + composed `YYYY/MM/DD/name` string (+ separate oid
alloc) per `.md`/`.mkd` blob across the whole repo tree — unbounded lwsac
per request from repo content (millions of crafted posts).  Recorded in
F-016's Status history.

## Checked clean

1. **blog scraping state machine** (:267-361): line walk bounded by
   `memchr('\n')` within the 4KiB scan window; title copy clamped; the
   `![alt](route)` parser's bracket/paren searches are all length-bounded
   within the line (`close_bracket[1]` may read the line-terminating
   byte — in-bounds of the odb buffer, which libgit2 allocates with a
   trailing NUL); summary appends guarded byte-by-byte against
   `sizeof(summary)-1` (the in-code proof comment is accurate); the
   capturing/done state machine cannot restart after completion.
2. **blog emission**: all four strings purified (the newest code follows
   the discipline); lwsac record layout exact (rlen+nlen+1);
   `oid_copy` in its own lwsac alloc keeps it stable across chunk growth;
   start-fail path calls `meta_trailer` before `meta_header` — benign
   (the mode guard returns early).
3. **log walk**: parent advance initializes `c = NULL` in the loop body
   (:118); count-limited walk ends safely via `!ctx->u.obj`; the 768
   reserve covers `commit_summary`'s verified ≤~330-byte emissions.
4. **repos**: ACL check under the vhost lock with unlock before `goto`;
   the 100+size space guard matches the actual item (name + three conf
   strings) — the sized-guard pattern blog should have used; reponame/
   desc/url purified, owner via `identity_json`.
5. **reflist**: iterator freed on all paths; dangling-symbolic-ref case
   frees gref and continues (the documented rref fix); ref names
   purified; `generic_object_summary` degrades to `{}` on lookup
   failure/odd types.

## Notes (not findings)

- blog.c logs each processed path at `lwsl_notice` — repo-controlled
  filenames into logs; control chars are bounded by the purifier? — no,
  the *log* line prints the raw `path` (not purified) at :242/:127 — log
  injection (newlines in filenames) — control-char log injection class;
  noted for the lib-main log-emission recheck already in the ledger.
- `job_blog` sets `ctx->partway = 1` manually — redundant with fill's
  own bookkeeping, harmless.

## Coverage

Full deep-read: `lib/job/blog.c` (399), `log.c` (128), `repos.c` (129),
`reflist.c` (109); witness: `commit_summary`/`generic_object_summary`
size bounds in util.c (lib-util's material, recheck stands).  Both units
→ `done`.  **Tier 2 complete** — next Track B unit is `lib-util`
(Tier 3, the escaping heart).

## Methodology notes

- None.
