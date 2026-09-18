# gitohashi Continuous Security Audit

Continuous, incremental security audit of gitohashi (libjsongit2), designed
to run unattended in the background without depending on any one person's
choice of topics.  See `METHODOLOGY.md` for the full process; in short:

- **Bounded passes** run on a schedule (twice daily) and on demand, each
  covering one or two components ("units").
- **Work selection is dual-track:** re-audit components with new commits since
  their last audit (Track A), else sweep the next unaudited component in
  attack-surface tier order (Track B).
- **State lives here:** `coverage.md` (what was audited against which commit),
  `findings.md` (index), `reports/` (one report per pass, findings with
  attack paths, severity and remediation sketches), `findings/` (self-contained
  per-finding artifacts `F-NNN.md`).
- **Audit passes are read-only on the repo sources.**  They never fix, never
  build, never touch git state.  Fixes happen in separately commissioned
  sessions after triage, and update the finding's `## Status history`.

gitohashi's distinct trust boundaries (vs the lws audit this process was
ported from): the unauthenticated HTTP client feeds `src/` routing and the
avatar proxy; **repo content** (commit messages, author identities, file and
ref names, blobs) pushed by anyone with push access is the second big
untrusted-input class, flowing through libgit2 → JSON composition
(`jg2_json_purify`) → clientside rendering (`assets/jg2.js`) — the central
injection chain of this codebase; gitolite config on disk is operator-trusted
but its parsed ACLs are the private-repo authorization boundary.

All artifacts are plain markdown so they are greppable, diffable and can be
committed or gitignored at the maintainer's discretion.
