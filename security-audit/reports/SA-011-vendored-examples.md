# SA-011 — assets-vendored + examples-corpus — 2026-09-13 — 2d61554

Units: `assets/showdown.min.js` + `highlight.pack.js` + css + `templates/gitohashi-example.html`; `examples/minimal/jg2-example.c` + `examples/threadchurn/threadchurn.c` + `xss/` corpus  |  Tier: 4  |  Tracks: B(sweep) ×2
Track A check: empty (HEAD = ledger commit 2d61554).  Working tree clean
(HEAD 2d6155448fbc9a74d1a61507c99731540d2d1e42).  Scope per the ledger:
vendored version/CSP glance + preprocessing-contract interaction only
(full vendored-lib audit out of scope).

## Build/entry-point framing

The vendored JS is served as same-origin static assets under
`/git/_gitohashi` (the template's script/link tags are all same-origin —
consistent with the strict CSP); the template injects generated meta +
JSON at the two `libjsongit2:` markers inside `<div id='initial-json'>`
(JSON is HTML_SAFE-purified server-side — SA-005-verified — so no
comment/tag breakout from repo content).  The examples are local CLI
tools over the public libjsongit2 api; the xss corpus is OWASP-derived
attack markdown served through the preprocessing contract.

## Findings

### F-022 [Low/medium] Vendored showdown 2.0.0-alpha1 (2018) and highlight.js 9.18.1 are years stale — downstream security fixes exist in both lines; current exposure is CSP-fenced (showdown) and client-side-hang class (hljs)
- Location: `assets/showdown.min.js` (/*! showdown v 2.0.0-alpha1 - 19-04-2018 */), `assets/highlight.pack.js` (/*! highlight.js v9.18.1 */)
- Reachability: every repo-content rendering path uses both — markdown (README/blog/blob) through showdown, code files through hljs.
- Preconditions / gating: the strict CSP fences showdown's known post-2.0.0 XSS hardening gaps (link-scheme/attribute vectors are script-src-blocked; the gitohashi preprocessing additionally neutralizes raw-HTML routes).  hljs grammar ReDoS is *not* CSP-relevant: a crafted code file can hang the viewing browser (fixes for ReDoS-class issues shipped in later 9.18.x/10.x lines).
- Attack path: repo pusher commits a code file shaped to a known post-9.18.1 hljs grammar ReDoS → viewer opens the file → `hljs.highlightBlock` hangs the tab (per-viewer client-side DoS).  The showdown leg is fenced twice (CSP + preprocessing) but depends on those fences persisting.
- Evidence: version banners in the vendored files; both upstreams shipped security releases after these versions.
- Impact assessment: hygiene/staleness with one live-ish consequence class (client-side hang) and one defense-in-depth dependence.  Low, medium confidence (no specific exploit PoC run in this pass — scope is the version glance).
- Remediation sketch: bump highlight.js to the current 10/11 line and showdown to the latest stable 2.x, re-run the ./xss corpus pages as the regression fence.

## Checked clean

1. **Template**: both `libjsongit2:` markers present exactly once; all
   script/style/font references same-origin (`/git/_gitohashi/...`,
   `/git/lws-login.js` — deployment-provided); no inline scripts or
   styles; the JSON-injection div relies on the (verified) server-side
   HTML_SAFE purification; CSP-consistent throughout.
2. **CSS**: no external references (the only `url()` is the same-origin
   font file); `logo.css` SVG data-URIs only.
3. **Preprocessing contract**: unchanged since SA-010's verification
   (`san_nq` before showdown, `hljs` on san'd text, `makeHtml` from
   `textContent`); the `./xss` corpus explicitly exercises the
   `' onmouseover='` vector class through the markdown path — note the
   corpus covers the showdown leg, while F-021's gap is in jg2.js's own
   repobar/breadcrumb path (corpus does not cover it).
4. **jg2-example.c**: correct api pairing (ctx create/fill/destroy,
   vhost destroy on all paths); local trusted argv input only.
5. **threadchurn.c**: bounded thread array (16); shares the vhost
   deliberately to churn the multithreaded paths (the F-020 class is
   exactly what it would exercise).

## Notes (not findings)

- threadchurn's `pthread_join` iterates all 16 slots even if a
  `pthread_create` failed mid-loop (garbage `pthread_t` join — test-tool
  robustness, resource-exhaustion-gated).
- The corpus README documents its own success criterion (no alert boxes)
  — a good manual fence to re-run after any escaper/vendored bump.

## Coverage

Glance-level per the unit scope: vendored version banners + template +
css line-read; examples full-read (292 lines); xss corpus head + purpose.
Both units → `done`.  **THE ENTIRE LEDGER IS NOW AUDITED — every unit in
all 4 tiers has a status; first full sweep complete.**

## Methodology notes

- None.
