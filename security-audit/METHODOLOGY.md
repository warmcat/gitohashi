# gitohashi Continuous Security Audit — Methodology

This is the authoritative process for the continuous, incremental security
audit of gitohashi.  It is followed identically by scheduled passes and by
manual invocations.  Tune the taxonomy or procedure here; never embed audit
logic into individual runs.

Companion files:

| File | Purpose |
|---|---|
| `coverage.md` | Component inventory (attack-surface tiers), last-audited commit per unit, sweep queue |
| `findings.md` | Global findings index with severity and status |
| `findings/` | Self-contained per-finding artifacts `F-NNN.md` — the live, citable work items |
| `reports/` | One markdown report per audit pass (`SA-NNN-<slug>.md`) — immutable pass history |

## Principles

1. **Read-only on sources.** A pass never modifies anything outside
   `./security-audit/`.  No fixes, no `git add`, no commits, no builds.  Fixes
   happen in separately commissioned sessions after human triage, and update
   the finding's `## Status history` (they may modify sources; audit passes
   never do).
2. **Incremental with state.** Every unit records the commit it was audited
   against.  Changed code re-enters the queue automatically.  No unit is lost,
   none is pointlessly repeated.
3. **Attack-path reasoning over pattern matching.** A finding without a
   reachability argument (who can feed this, with what preconditions, gated by
   which build/config options) is not a finding.  gitohashi's severity depends
   heavily on *which trust boundary* feeds the code: unauthenticated HTTP
   client > repo pusher (content) > gitolite admin > vhost operator.
4. **Bounded passes.** Deep reading beats broad skimming.  Stop after the
   budget (below) and record partial progress honestly.
5. **Respect AGENTS.md** at the repo root if one is present.  It applies to
   audit runs too.

## Work selection (each pass)

Determine the unit(s) to audit, in priority order:

- **Track A — change-driven:** for each unit in `coverage.md` with
  `status = done` or `partial`, run
  `git log --oneline <last-audited-commit>..HEAD -- <unit paths>`.
  Units with new commits are re-audit candidates; prioritize by tier.
  Only audit the delta plus enough surrounding context to reason about it,
  but read the unit's entry points as needed.
- **Track B — systematic sweep:** if Track A yields nothing (or its delta is
  trivially small, e.g. comment/log-only changes — say so in the ledger),
  take the lowest-tier `status = never` unit from `coverage.md`.

**Budget:** one pass covers up to ~2 units, or ~5k loc of deep-read, whichever
comes first.  Units larger than ~5k loc are audited across multiple passes;
mark the unit `partial` and record exactly which files/areas were covered.

Selection and last-audited commits are computed against **HEAD** (committed
state).  If `git status` shows uncommitted modifications inside a selected
unit, still audit, but stamp the report `working-tree-dirty` and mark findings
from uncommitted regions accordingly — they are auditing WIP, not releases.

## Audit procedure (per unit)

1. **Frame the unit.** Note the build options that gate it
   (`GOH_LWS_PLUGINS` — protocol plugins vs the built-in daemon path;
   `JG2_HAVE_ARCHIVE_H` — snapshot.c vs the no-snapshot.c shim; libgit2
   version capability guards), which of the two lws integrations reaches it
   (daemon `src/main.c` static protocols vs runtime plugins), and which
   vhost-config options enable the surface (etc-gitohashi JSON).  Record this
   in the report — it drives severity.
2. **Map entry points.** Identify every path by which data reaches this unit:
   the HTTP client via lws callbacks (URL, urlargs, headers), the lws
   threadpool service thread, repo content via libgit2 calls, gitolite conf
   from disk, the avatar provider's HTTP responses, on-disk cache files.
   Note the trust level at each entry.
3. **Deep read with the taxonomy** (below) against each entry path.  Prefer
   tracing concrete data flows end-to-end (buffer limits, length types,
   truncation points, allocation sizes vs copy sizes) to skimming for shapes.
4. **Verify each candidate finding:** re-read the surrounding code and callers
   until you can state the full attack path, or drop it.  Check whether an
   existing lws helper already mitigates it (`lws_snprintf` truncation
   semantics, `lws_json_purify`, `lws_urlencode`, `lws_diskcache_*`,
   `lws_threadpool` task-status contract) — misuse of those helpers is in
   scope; absence where they should be used is a finding with a remediation
   sketch.
5. **Check web assets** if the unit contains client-side JS/HTML
   (`assets/`, `templates/`): strict CSP means no inline scripts or styles,
   no `eval`; every repo-controlled string composed into the DOM must pass
   the established escaping path; check the showdown/highlight preprocessing
   contract (README "XSS mitigation") is applied at every feed site.
6. **Write the report** (format below) into `reports/`, using the next free
   `SA-NNN` sequence number (look at the existing reports).
7. **File each finding** as a self-contained `findings/F-NNN.md` (next free
   id from `coverage.md` bookkeeping): YAML front matter (id, title, severity,
   confidence, status, unit, flaw-unit if cross-unit, files, audited-at-commit,
   filed, report, recheck-by) then the full detail — location, gating, attack
   path, evidence, impact, remediation sketch, empty `## Status history`.
   Then add the row to `findings.md`.  If a finding duplicates an existing
   open one, do not create a new id; append a dated note to the existing
   finding's `## Status history` and mention it in the report.
8. **Update `coverage.md`** — set `last-audited` date, commit (`git rev-parse
   HEAD`), and status (`done` / `partial` with notes).

## Vulnerability taxonomy (gitohashi-specific)

Ordered roughly by expected yield in this codebase.

1. **Client-controlled request intake.** URL routing and urlarg parsing in
   `src/protocol_gitohashi.c` (repo name, ref, in-repo path, page/size
   args, ETAG/If-None-Match handling, the bot user-agent probe) and the
   avatar-proxy request path.  Hunt: missing sanitization before use in
   paths or libgit2 calls, unbounded urlarg values, confusable route
   matching, cache-key composition from client strings.
2. **Repo-content → JSON → HTML injection chain** (the central chain):
   commit messages/summaries, author names and emails, tag messages, file
   and dir names, ref names, blob contents — all attacker-influenced by
   anyone with push access.  Follow each through `jg2_json_purify()` /
   `ellipsis_purify()` / `lws_json_purify_flags()` into JSON, then:
   truncation that splits a JSON escape sequence at the ellipsis boundary,
   sandwich-mode injection of the JSON blob into the HTML template at the
   `<!-- libjsongit2:initial-json -->` / meta markers (does `</script>` or
   `-->` in repo content break out?), snapshot `Content-Disposition`
   filename composition, response headers built from repo-controlled
   strings, log injection (repo names/refs into lws logs).
3. **Clientside rendering & vendored-preprocessing contract.**
   `assets/jg2.js` innerHTML-class sinks (10 sites at last count) — every
   interpolation of JSON data must be escaped or textContent'd; the
   showdown/highlight preprocessing (README "XSS mitigation" character
   escapes) must cover every path that feeds them (markdown blobs, file
   contents, names); check `esc()` coverage and attribute-boundary breakouts
   (quotes in href/title attributes), URL composition from repo strings
   (open-redirect / scheme injection in generated links).
4. **Threading & lifetime.** The lws threadpool jobs (service-thread
   `task_function` vs event-loop wsi lifecycle): dangling wsi after HTTP
   client disconnect (`lws_threadpool_get_task_wsi` NULL handling — recent
   fix family on this branch), STOPPING/503 shedding, job/ctx reuse across
   queued requests; libgit2 object lifetimes (`git_object_free` pairing,
   `git_buf`/`git_str` disown semantics, repo open/close across threads);
   `jg2_ctx` buffer reuse between cache fill and send.
5. **Integer & buffer safety.** `lws_snprintf` returns the *untruncated*
   length — any `p += lws_snprintf(...)` cursor advance past `end` (the
   classic cursor-overshoot OOB write class, cf. lws F-059) including via
   the `CTX_BUF_APPEND` macro; `JG2_HAS_SPACE` reserve accounting; size_t
   vs int len types at libgit2 boundaries; allocation size vs copy size on
   every malloc/realloc; off-by-one on NUL terminators.
6. **Path traversal & file operations.** The repo-name sanitize
   (`lib/util.c`, "sanitize url-provided repo name against .. attack") and
   its callers; tree/plain in-repo path handling; blog-mode `YYYY/MM/DD`
   traversal; snapshot temp-file creation and naming via libarchive;
   `lws_diskcache` paths for JSON and avatar caches (email-hash-derived
   filenames); template file reads; anything composing a filesystem path
   from repo names or ref names.
7. **Avatar proxy outbound handling.** URL composition from commit emails +
   provider templates (query injection into the provider URL), response
   size/type validation before caching, cache poisoning via crafted
   provider responses or hash collisions in naming, fetch flooding
   (attacker commits many identities → outbound fetch amplification),
   failure-mode behavior.
8. **Access control.** gitolite ACL parsing correctness
   (`lib/conf/gitolite3.c`) — a parse defect that misclassifies a repo as
   readable exposes private repos (the authz boundary; elevate severity);
   vhost `acl_user` mapping; repo discovery (scan-repos) vs ACL application
   ordering; cached-JSON leakage of a repo that became private (cache
   invalidation on ACL change, not just ref change).
9. **Resource exhaustion / DoS.** Threadpool queue depth and the shedding
   behavior; huge blob/plain serving; search cost on large repos; cache disk
   growth and LRU reaping; avatar cache disk; unbounded listings (refs,
   tags); per-connection memory retention while jobs run.
10. **Privacy & disclosure.** Committer emails/IPs in logs or JSON where not
    needed (the avatar proxy exists specifically to avoid third-party
    referrer leaks — keep it that way), private-repo ref names observable
    via cache timing or shared cache keys, error paths that reveal
    filesystem layout.
11. **Build-dimension traps.** `GOH_LWS_PLUGINS=OFF` (default daemon path)
    vs `ON` (plugins built from the same `src/protocol_*.c` — both must be
    safe); missing `JG2_HAVE_ARCHIVE_H` (no-snapshot shim must fail closed,
    not 404 into weirdness); libgit2 version guards; `GOH_WITH_ASAN` only
    affects instrumentation, never logic.

## Severity and confidence

| Severity | Meaning |
|---|---|
| Critical | Unauthenticated HTTP client or any repo pusher, default deployment: memory corruption, authz bypass (private repo exposure), active XSS in the gitohashi origin |
| High | Same impact but gated behind a non-default build/config; or practical remote DoS on the default deployment (threadpool wedges, disk exhaustion) |
| Medium | Unusual config or privileged position required (gitolite admin, vhost operator); security-boundary hardening failure; DoS behind non-default options |
| Low | Defense-in-depth and hygiene issues with plausible future exploitability |
| Info | Not a vuln today, but a pattern worth recording |

Every finding carries a confidence (`high` / `medium` / `low`) and the
reasoning behind it.  Low-confidence criticals are fine to file — say so.

## Report format

File name: `reports/SA-NNN-<unit-slug>.md` (NNN zero-padded, next free).

```markdown
# SA-NNN — <unit> — <UTC date> — <short commit> [working-tree-dirty?]

Unit: <path(s)>  |  Tier: <n>  |  Tracks: A(delta from <commit>) / B(sweep)
Build gating: <options, default-build status, daemon/plugin reachability>

## Findings

### F-NNN [SEVERITY/confidence] <title>
- Location: `file:line`
- Reachability: <who feeds this, entry path>
- Preconditions / gating: <build options, config>
- Attack path: <step by step>
- Evidence: <minimal code excerpt>
- Remediation sketch: <approach, not a patch>

## Checked clean
<taxonomy classes actively checked and not found, so the next pass
knows what was covered>

## Coverage
<files/areas deep-read; if partial, exactly what remains>
```

## Finding lifecycle and references

Finding ids `F-NNN` are the canonical, stable handles — unique, never reused,
resolvable by any context with repo access via `findings/F-NNN.md` alone
(that file is deliberately self-contained: no report or conversation context
needed to act on it).  Report ids `SA-NNN` reference passes; unit ids (the
ledger keys, e.g. `proto-gitohashi`) reference components.

`findings/F-NNN.md` is the **live** document: status changes, triage
decisions, and fix commits are appended to its `## Status history`; its front
matter `status:` is kept in sync, along with the index row.  Reports are
**immutable** history once written.

Commissioned fix sessions (which, unlike audit passes, may modify sources)
update the finding they fix: set `status: fixed`, add the fixing commit to
`Status history`, and update the index row.  Recommended commit-message
convention repo-wide: reference ids as `F-NNN` (e.g. `search: clamp page
size, fixes F-004`) so git log greps both ways.

## Hard rules

- Sources are read-only.  Writes are confined to `./security-audit/`.
- Never run builds; never touch `./build` or any `build-*` dir.
- Never modify git state (commits, index, branches, `.git` anything).
- Never copy live committer PII (real emails) or secret material into
  reports; refer to it abstractly.
- A pass that finds nothing still writes its report and ledger updates —
  coverage and non-findings are results too.
- If something about this methodology is wrong or missing, note it in the
  report under `## Methodology notes` for human follow-up; do not improvise
  structural changes silently.
