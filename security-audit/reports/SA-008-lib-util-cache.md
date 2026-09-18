# SA-008 — lib-util + lib-cache — 2026-09-13 — 2d61554

Units: `lib/util.c` (1273 loc); `lib/cache.c` (169, read SA-003) + `lib/repostate.c` (272)  |  Tier: 3  |  Tracks: B(sweep) ×2
Track A check: empty (HEAD = ledger commit 2d61554).  Working tree
clean (HEAD 2d6155448fbc9a74d1a61507c99731540d2d1e42).

## Build/entry-point framing

lib-util is the escaping heart of the taxonomy-2 chain: URL splitting +
sanitize, the purify wrappers (`jg2_json_purify`/`ellipsis_purify`/
`ellipsis_text`), the RFC 2047 name decoder, identity/signature JSON,
oid+alias emission, object summaries, and `blob_from_commit`.  Inputs:
raw (still-percent-encoded) URL from the proto-gitohashi stash, and repo
content (signatures, tag messages, ref names) via libgit2.  lib-cache:
the diskcache query wrapper (md5-keyed, `-suffix` namespacing, vsnprintf
clamped) and the ref-state machinery — `__repo_reflist_update`'s
incremental coherence walk + md5_refs recompute, run under vhost+jrepo
locks from ctx create (service thread) and the 1s cache-trim thread.

## Findings

### F-019 [Low/high] Duplicate urlarg keys leak the earlier strdup — `?h=a&h=b` (or `id=`/`q=`) leaks one bounded allocation per request, unauthenticated
- Location: `lib/util.c:141-180` (`jg2_repopath_split`'s param loop: each matching key strdups and assigns `sr->e[...]` unconditionally)
- Reachability: any unauthenticated request with a repeated `h`, `id` or `q` parameter — the second assignment orphans the first strdup (freed only via `jg2_repopath_destroy`, which sees just the final pointer).
- Preconditions / gating: none — default build; the param loop parses up to 4 params of any combination.
- Attack path: `GET /git/repo/log/x?q=aaa&q=bbb` (repeat) → each request leaks `strlen("aaa")+1` bytes; URL-bounded (~≤1KB), unbounded request rate → slow memory growth, same leak class as F-011/F-012.
- Evidence: `sr->e[JG2_PE_BRANCH] = pp;` etc. with no `free()` of a prior value; every other field of `sr` is written exactly once.
- Impact assessment: slow unauth memory leak.  Low, high confidence.
- Remediation sketch: `free((char *)sr->e[...])` before reassignment (or skip duplicate keys after the first).

## Closed rechecks (standing items)

1. **SA-001's repopath recheck — CLOSED**: `jg2_repopath_split` performs
   no percent-decoding (elements stay encoded — `%2e%2e` stays literal
   and simply fails repo/entry lookup), and the `..` sanitize rejects
   any adjacent dots in both NAME and PATH (:85-111, conservative
   any-position scan).  Functional note: for URLs with no `/` after the
   repo name, the NAME-stage scan covers the *whole* remaining string
   including urlargs, so a benign `?q=v1..v2` on a repo-list URL is
   rejected as "illegal .." (false-positive reject, not a bypass).
2. **F-008's sr.e[] shape recheck — CLOSED**: mode/vid/path are
   free-form raw substrings; the emission side is purified (580bd3b,
   SA-005-verified).
3. **SA-006/SA-007's util recheck — CLOSED**: `blob_from_commit`
   (bounded branch compose ≤127+NUL, fail-closed ref/oid lookups, exact
   tree-entry frees), `signature_json`/`name_email_json` (both name and
   email through `ellipsis_purify`; RFC 2047-decoded names still
   purified), `identity_json` (clamped strncpy parses, all outputs via
   name_email_json), `signature_text`/`ellipsis_text` (C0/DEL→space
   fences header injection in text mode), `commit_summary` (100-byte
   purified summary + bounded signatures ≈ ≤330 bytes, matching the
   768 reserves) — all verified.
4. **F-007's key-equality recheck (lib-cache side) — CLOSED**: cache.c
   adds nothing vhost-specific to repo-affiliated keys (the `-suffix`
   namespaces object kinds only); the exposure is fenced at the avatar
   mount + conf level as fixed in 2d61554.

## Checked clean

1. **Purify family**: `jg2_json_purify`'s len/in_used contract (0-cap =
   process nothing for the diff streamer; >0x7fffffff clamped);
   `ellipsis_purify`'s `max-3` headroom exactly covers the `"..."`+
   NUL memcpy (verified against the lws purifier's exit-at-len≤6 bound);
   truncation can never split an escape.
2. **RFC 2047 decoder** (the json-purify-improvements code): strictly
   validated — charset allowlist (utf-8/us-ascii/latin1), b64/Q charset
   gates, `?=` terminator required with all reads in-bounds (NUL is
   readable and fails the match), raw bytes capped at tmp/2 so the
   latin1→UTF-8 in-place doubling cannot overflow, `raw_len >= tmp_len`
   final fence, decoded-once (result never rescanned), strict UTF-8
   validation (overlong/surrogate/C0/U+10FFFF caps — canonical shape),
   all out-copies byte-bounded.
3. **`__repo_reflist_update`** (repostate.c): the hash-chain
   snip/prev-pointer invariant holds — `er_hash_prev[bucket]` always
   points at the slot after the last-processed entry of that bucket,
   which is exactly the predecessor slot of any later same-bucket entry
   **given libgit2's deterministic name-ordered ref iteration** (recorded
   as a constraint: the correctness of the incremental patch depends on
   iteration order stability across walks); symbolic refs skipped
   without deref (both loops, the documented SIGSEGV fix); coherence
   breaks snip + free exactly the orphaned tail; the md5 recompute runs
   under vhost+jrepo locks so the shared `vh->md5_ctx` is serialized;
   3s throttle bounds rewalk frequency; refchange cb is the no-op logger
   (safe from the trim thread).
4. **cache.c** (SA-003 read, re-checked): vsnprintf would-write clamp,
   suffix namespacing bounded (`buf[256]`), trim thread walk under the
   global lock.

## Notes (not findings)

- `jg2_repopath_split` returns success (0) on a param missing `=`
  (:133-135) — partial params accepted; harmless.
- OOM path in the regenerate loop (`repostate.c:186-187` `goto bail`)
  leaks the reference iterator (one-shot, OOM-gated).
- Huge-ref-count repos pay a full ref walk per 3s on the trim thread —
  bounded by strcmp cost; noted as F-016-adjacent amplification only.
- `otype_name` bounds-checked; `jg2_json_oid` alias list capped at 8 with
  purified ref names under the jrepo lock.

## Coverage

Full deep-read: `lib/util.c` (all 1273 lines), `lib/repostate.c` (272);
`lib/cache.c` re-checked from SA-003.  Both units → `done`.  Next Track
B: `lib-main`.

## Methodology notes

- None.
