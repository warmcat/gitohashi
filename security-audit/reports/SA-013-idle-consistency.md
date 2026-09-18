# SA-013 — idle pass + audit-artifact consistency check — 2026-09-17 — 2d61554

Selection: Track A empty (HEAD = 2d6155448fbc9a74d1a61507c99731540d2d1e42,
every unit recorded there; no source deltas since SA-012).  Track B
exhausted (ledger complete since SA-011).  Standing-recheck queue empty
(SA-012 resolved the last actionable item).  No unit re-audited; this
pass used its budget for a consistency self-check of the audit
artifacts, given the number of scripted ledger edits across SA-005..SA-012.

## Consistency results (all pass)

- Findings: `findings/F-001.md`…`F-023.md` contiguous, 23 files; index
  rows in `findings.md` match the file set exactly (no missing/dup ids);
  front-matter `status:` agrees with the index (15 fixed, 8 open =
  F-016…F-023).
- Reports: `SA-001`…`SA-012` contiguous, one per pass.
- Coverage ledger: 18/18 units `done`, 0 `never`, 0 true `partial`
  (the two in-row "partial" grep hits are the phrases "partial-fail" /
  "partial-write" inside checked-clean notes).
- Bookkeeping counters verified against the artifacts: next free
  SA-014 / F-024 after this pass.

## Findings

None — no source deltas to audit.

## State summary for triage

Open findings awaiting maintainer decision (all Low except F-016):

- F-016 (Medium) unbounded diff/lwsac materialization (commit/patch
  views; tree/blog/blame variants)
- F-017 blog item reserve → cached truncated JSON
- F-018 uninitialized `c` in job_log end-block
- F-019 duplicate-param strdup leak
- F-020 rei lifetime lock asymmetry (rd vs vh lock)
- F-021 jg2.js quote-escaping gap + raw URL interpolation
- F-022 vendored showdown/hljs staleness
- F-023 60s one-shot response cap (re-arm remediation)

## Methodology notes

- Idle passes with a consistency check are a reasonable steady-state
  behavior once the sweep is complete and the recheck queue is empty;
  if they recur with nothing to verify, consider lengthening the
  schedule or gating the automation on new commits landing.
