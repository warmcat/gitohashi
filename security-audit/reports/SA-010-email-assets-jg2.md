# SA-010 — email + assets-jg2 — 2026-09-13 — 2d61554

Units: `lib/email/email.c` (203) + `md5.c` (312, vendored mbedtls MD5) + `email/private.h`; `assets/jg2.js` (2683)  |  Tier: 3 + 4  |  Tracks: B(sweep) ×2
Track A check: empty (HEAD = ledger commit 2d61554).  Working tree
clean (HEAD 2d6155448fbc9a74d1a61507c99731540d2d1e42).

## Build/entry-point framing

email: the identity→md5 cache (LRU bins, vhost lock) feeding gravatar
hashes and the avatar cb; md5.c is a vendored mbedtls MD5 (fixed-struct
implementation, memcpy bounded by the 64-byte block math — grep-verified
shape; non-cryptographic use by design: cache keys + gravatar).  jg2.js:
the clientside renderer — JSON (server-purified, HTML_SAFE `\u00XX`) →
DOM via string-built innerHTML with local escapers `san`/`san_nq`, the
showdown (`sd_ext_plain`) and highlight.js preprocessing contract, search
UI, and the URL breadcrumb.

## Findings

### F-021 [Low/medium] jg2.js escaping gaps: `san()`/`san_nq()` do not escape single quotes while several attributes are single-quoted with repo content, and the repobar breadcrumb/makeurl hrefs interpolate raw `rpath`/`qbranch`/`qid` from `window.location` — CSP-fenced markup injection (unauthenticated + repo-pusher legs)
- Location: `assets/jg2.js:393-414` (`san`/`san_nq` escape `& < > " %` — not `'`); single-quoted attribute sinks with repo content: `:1342-1345` and `:1351-1354` (tree entry hrefs — filenames), `:919` (identity `alt` — emails), `:964-965`/`:976-977`/`:1108-1109` (aliases/patch_archive `tgt` — branch/tag names); raw-URL sinks: `:2125-2150` (breadcrumb `e[n]` unescaped text + `makeurl(..., rpath, ...)` into double-quoted hrefs), `:2442-2525` (`rpath`/`qbranch`/`qid` are raw `window.location` slices, never escaped)
- Reachability: leg (a) repo pusher — git tree entry names, branch/tag names and commit emails can legally contain `'` (git refname rules allow quotes; filenames allow anything but '/' and NUL).  Leg (b) unauthenticated — any URL under an existing repo: `rpath` is a raw client-side slice of the location (the server's 403/404 does not prevent it: the repobar renders before the error item, and JSON arrives for any resolvable repo).
- Preconditions / gating: strict CSP (`script-src 'self'`, no unsafe-inline) fences both legs to passive markup/attribute injection — injected event handlers and inline styles do not execute; link hrefs can be reshaped (click-phishing inside the origin).
- Attack path: (a) push a branch named `x'-something` → `aliases()`'s `tgt='…'` closes early → injected attributes/markup in every log/commit decoration; (b) `GET /git/<real-repo>/tree/a"><img src=x onerror=…>` → rpath reaches the breadcrumb href/text raw → markup injection in the rendered page for the requesting viewer.
- Evidence: `san()` replacement list (:398-402) vs the single-quoted attributes at the listed sinks; `e[n]` and `makeurl()` args never pass through `san` at :2125-2150.
- Impact assessment: CSP-gated DOM injection (no script execution; content/link spoofing) — same grading as F-008.  Low, medium confidence (escaper gap and raw sinks code-verified; browser testing out of scope for this pass).
- Remediation sketch: add `\'`→`&#39;` to `san`/`san_nq`; route the breadcrumb components and all `makeurl` string args through `san`; keep double-quoted attributes for anything URL-shaped.

## Closed rechecks

1. **SA-002's email recheck — CLOSED**: the bins cache is bounded
   (bins × depth, default 16×16, email[64] entries) with correct
   move-to-front recycling incl. the documented depth-1 head-case fix;
   `JG2_EMAIL_MAX_LEN` (256) caps both the hash sum walk and the md5
   input (the documented CPU bound); avatar cb invoked under the vh lock
   with a one-directional lock order (vh → avatar vhd — no inversion
   exists); OOM NULL-handled downstream (`?` md5).
2. **SA-004's F-014 DOM-sink recheck — CLOSED**: every JSON-derived
   string at the render sinks passes `san`/`san_nq`/`identity()`
   (search fp/ac tokens san'd at :1866-1868; blog fields san'd; blob
   content san_nq'd before showdown/hljs) — injected JSON members cannot
   reach the DOM unescaped except via the F-021 quote/raw-URL gaps.

## Checked clean

1. **Purify double-fence**: server-side HTML_SAFE `\u00XX` purification
   (SA-005-verified) + client-side `san` at the sinks — the F-014 chain
   is closed at both ends.
2. **showdown/hljs contract**: markdown blobs are `san_nq`'d (with the
   zero-width-space `<` defeat for showdown's entity re-interpretation)
   before `#do-showdown`, then `makeHtml`'d from `textContent`
   (:2258-2294 — reads the escaped text, not raw markup); hljs operates
   on `san`'d code text; blog summaries go through `conv.makeHtml` of
   the purified summary or `san` fallback (:1514).
3. **Identity/avatar URLs**: `identity_av_base` composes operator pvo +
   hex md5 only (double-quoted src); `identity()` sanitizes email and
   name (decodeURIComponent wrapped in try/catch for malformed
   sequences).
4. **i18n tables**: fixed-string constants only.
5. **Aging/menu/copy machinery**: numeric or constant interpolations;
   clipboard via textarea value (no HTML sink).
6. **email.c**: vhost deinit frees all bin nodes; `bins`/`depth` zero
   defaults applied before first use (note: a library consumer setting a
   nonpositive `email_hash_bins` would mis-index — no in-tree setter).

## Notes (not findings)

- Emails longer than 63 chars never cache-hit (stored copy truncated at
  63 vs full input strcmp) — correct results, just uncached; the md5 is
  computed on up to 256 bytes as documented.
- `jg2.js:100` `lobalContext` typo in the vendored i18n (dead
  clearContext path); `doc_dir` at :1503 leaks a global — cosmetic.
- `makeurl` itself does not escape — safe only where callers pre-san
  (the F-021 remediation should centralize this).

## Coverage

Full deep-read: `lib/email/email.c`, `email/private.h`; `md5.c`
shape-verified (vendored crypto, fixed-struct); `assets/jg2.js` all 2683
lines (i18n block skimmed as constants; render/sink paths line-read).
Both units → `done`.  Remaining never-units: `assets-vendored`,
`examples-corpus` (Tier 4).

## Methodology notes

- None.
