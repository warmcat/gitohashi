# SA-016 — job-ssr (new unit: SSR renderer) + fix-delta verification — 2026-09-18 — 3faf679

Units: NEW `job-ssr` = `lib/job/ssr.c` (2788) + `lib/job/markdown.c` (906) + `lib/job/jsondom.c` (530), plus the SSR-era glue in `lib/job/job.c` / `lib/private.h` (capture plumbing, locale cache key) read as context  |  Tier: 2  |  Tracks: A(the 2d61554 → 3faf679 delta)
The delta also contains the 2026-09-18 fix session's 8 commits — verified below — and the maintainer's cb9629a/b1623fb/2f75a1e/6b2f24a/0194127/1eb2422, of which the SSR trio is the substantive new attack surface and consumes this pass's budget.  Working tree clean (HEAD 3faf679914a6e2628ec99f786ace760c36e3b02a).

## Fix-delta verification (8 commits, all sound)

90bd8f9 (F-018 `c`-init + success gate), 9d1829b (F-019 free-before-reassign),
f352288 (F-017 post-purification sized guard), d4e03fb (F-023 write-path
timeout re-arm), 330a42a (F-021 jg2.js removal + install), fef4db4 (F-022
vendored removal + README), 8e39436 (F-020 generations), 5ecfd1b (F-016
caps) — committed shapes re-read; the F-020 generation readers are
integrated with the SSR-era job.c (item-8 / hash_visible_repos walk
`ctx->rei_gen`) and the locale byte enters the cache key as the parsed
0-3 enum, not the raw header.  Session verification (builds, 51-commit
hazard repo, 16-thread churn) stands; tree unchanged since.

## Build/entry-point framing (job-ssr)

For HTML contexts the jobs' JSON is captured into a growing per-ctx
buffer (doubling, 32 MiB cap, overflow degrades to a "use the plain
view" notice), parsed by jsondom into an lwsac DOM, and rendered to
HTML mirroring the old jg2.js output; the cached artifact is the
rendered HTML (locale-keyed).  Inputs: the JSON (repo content +
URL-derived strings, server-side HTML_SAFE-purified upstream), URL
elements via srr_init, operator config (vpath/avatar base).  Naked
endpoints (plain/patch/ac) bypass SSR.

## Findings

### F-024 [Medium/high] markdown.c recursion is depth-unbounded — a crafted README/blog (a single long `>`-prefixed line, or nested emphasis) drives unbounded md_lines/md_inline recursion: stack exhaustion on every view
- Location: `lib/job/markdown.c:619-647` (blockquote branch: each level strips one `>` and recurses into `md_lines`), `:353-381` (nested `**`/`*` recurse into `md_inline`); no depth parameter anywhere
- Reachability: repo pusher commits a README/blog article containing one long line of `>` characters (or deeply nested emphasis); every unauthenticated view of the README/tree/blog page renders it.
- Preconditions / gating: none beyond defaults — the markdown path is the default README view; the 2048-line cap does not help (a single line suffices), and blob size is bounded only by the 32 MiB capture.
- Attack path: `jg2_markdown` splits the blob into lines → `md_lines` sees `s[0] == '>'` → strips one `>` and recurses on the remainder → recursion depth equals the number of leading `>` characters in that one line (or the emphasis nesting depth) → stack exhaustion → worker SIGSEGV; repeatable per request (the page is not cached on failure, so every hit re-crashes).
- Evidence: `md_lines` blockquote branch calls itself on `inner[]` with no depth accounting; `md_inline` likewise recurses through strong/em/link-text without a cap.
- Impact assessment: same class as F-016 (repo-content-gated, unauth-detonated service DoS) but harder-lived: a crash per view rather than memory pressure.  Medium, high confidence (mechanism code-verified; trivially constructible input).
- Remediation sketch: thread a depth counter through `md_lines`/`md_inline` (and the resolver-independent paths) bailing to literal text past e.g. 16 levels — blockquote/emphasis nesting beyond that is not real content.

### F-025 [Low/medium] markdown.c's URL scheme gate is case-sensitive — `JaVaScRiPt:` / `DATA:` URLs slip the blacklist and reach the href (CSP-fenced, but contradicts the "raw XSS class removed entirely" contract)
- Location: `lib/job/markdown.c:119-129` (`strncmp` against lowercase `"javascript:"`, `"data:"`, `"vbscript:"` and the passthrough prefixes)
- Reachability: repo pusher writes `[x](JaVaScRiPt:alert(1))` in a README/blog; the scheme checks miss the mixed-case form, it fails the resolver prefixes too, and the belt-and-braces percent-encoder passes it raw (letters and `:` are url-safe) into `href="..."`.
- Preconditions / gating: the strict CSP (`script-src 'self'`) blocks javascript:/data: execution — fenced exactly like the F-021/F-008 classes; but the markdown.c header contract says raw HTML never passes through precisely to "remove the XSS class entirely", and this is an live injection route that survives that claim.
- Attack path: click-phishing/markup injection only under CSP; any CSP loosening re-arms script execution.
- Impact: Low (CSP-fenced markup injection); medium confidence (verified by reading; no browser PoC this pass).
- Remediation sketch: case-insensitive scheme comparison (lws_snprintf the first 11 chars lowercased, or strncasecmp), or invert to an allowlist — only http(s)/mailto/#// plus resolver-relative pass, everything else becomes `#`.

## Checked clean

1. **Escaping discipline (the central question)**: every repo-content
   and URL-derived interpolation in ssr.c routes through `ssr_esc` /
   `ssr_esc_attr` (escaping `& < > '` always, `"` in attr mode — the
   F-021 lesson applied server-side, correctly paired with both its
   single- and double-quoted attribute styles) or `url_make` /
   `ssr_urlenc_len` (percent-encoding everything not unreserved);
   verified across repolist, identities, aliases, log/commit/tags/
   branches, tree listing, blob views, blame titles, search results,
   the repobar/tab chrome, breadcrumb (`agg[512]` bounded, segments
   escaped) and the search-form `value='...'`.
2. **jsondom.c**: strict and bounded — depth cap 24, worst-case-sized
   string decode buffers, `hex4`/surrogate bounds checks, defensive
   `utf8_emit` capacity fence, NUL inside every allocation.
3. **markdown.c otherwise**: raw HTML never passes (every text byte
   through md_esc); fenced/indented code, tables, headings, lists all
   escaped; link/image destinations via md_url with percent-encoding
   after the resolver; line array capped (2048), blockquote inner cap
   (128).
4. **Capture/render plumbing**: 32 MiB capture cap with clean overflow
   degradation (explicit "use the plain view" error); the html cache
   key takes the locale as one parsed enum byte (no header-string
   injection into the key); cached-HTML spool skips the `]}`-JSON-splice
   fixup; `jg2_ssr_drain` writes only the cacheable prefix
   (`ssr_cache_len`) and fails closed on short writes; stats tail kept
   uncached.
5. **Renderer bounds**: reflist/branches/tags/posts capped (512), log
   50 rows, contrib strip 20, blamemap `ln < lines` fence.
6. **lws-hl integration**: the C-language tokenizer emits through the
   lws html sink (escaped text in hl spans, lws-side audited); fallback
   paths escape; blame groups partition the file so the persistent
   tokenizer stream stays byte-exact.

## Notes (not findings)

- `jg2_hbuf` grows unboundedly, but every input is capped (32 MiB
  capture, 8 MiB diff, 50k listing entries), bounding worst-case
  rendered output structurally (~6-8x escapes) — large but finite per
  request; revisit only if the caps change.
- blamemap stores hunk indices in int16 — files with >32767 blame
  hunks wrap to negative and lose shading (cosmetic, no memory issue).
- `jg2_jn_ll` uses atoll and accepts STR nodes — all current callers
  feed emitter-generated numerics; keep it that way.
- The markdown "2048 lines" cap silently truncates very long documents
  (functional).

## Coverage

Full deep-read: jsondom.c (530), markdown.c (906), ssr.c (2788, all
regions incl. hl glue, capture/drain, stats, page assembly); job.c
SSR-era diff read as context.  New ledger row `job-ssr` → `done` at
3faf679; the fix-touched unit rows bumped to 3faf679 with verification
notes.  Next free: SA-017 / F-026.

## Methodology notes

- New unit row `job-ssr` added for the SSR trio (lib/job/ scope): the
  ledger had no row covering these new files; recorded here rather
  than improvising silent coverage.
