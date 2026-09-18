# SA-006 — job-blame-commit + job-tree-plain — 2026-09-13 — 2d61554

Units: `lib/job/blame.c` (540 loc) + `lib/job/commit.c` (376); `lib/job/tree.c` (503) + `lib/job/plain.c` (81)  |  Tier: 2  |  Tracks: B(sweep) ×2
Track A check: empty (HEAD = ledger commit 2d61554).  Build gating:
blame needs `LIBGIT2_HAS_BLAME` (libgit2 ≥0.21, the normal build; the
no-blame dimension stubs to `final=1`); commit's diff cb shape follows the
libgit2 version; everything default-build.  Working tree clean (HEAD
2d6155448fbc9a74d1a61507c99731540d2d1e42).

## Build/entry-point framing

All four are jg2 jobs on threadpool workers fed by URL path elements and
repo content: blame (blame view + the blame-after-tree chain), commit
(commit summary + body + diff, or raw `patch` text), tree (dir listings +
inline blob view + README inline detection), plain (raw blob serving).
The commit/patch path materializes the whole diff into a per-ctx lwsac
before streaming; tree collects a dir level into lwsac; plain streams
directly from libgit2's mmap of the object file.

Verified supporting contracts: `lwsac_sizeof(first)` =
sizeof(lwsac) + (first ? sizeof(lwsac_head) : 0) (lwsac.c:63-66) —
commit.c's first-chunk `lwsac_sizeof(1)` vs transition `lwsac_sizeof(0)`
record offsets are exactly right; libgit2 `git_blob_rawcontent` is
mmap-backed (no heap copy for plain/blob streaming); the SA-005-verified
purifier backbone (HTML_SAFE `\u00XX`) backs every repo-content emission
in these files.

## Findings

### F-016 [Medium/medium] Commit/patch views materialize the entire diff into an unbounded per-request lwsac — repo-pusher plants a huge-text diff, any unauthenticated view request detonates it (memory exhaustion / AS-limit pressure on the whole daemon)
- Location: `lib/job/commit.c:183-201` (`git_diff_tree_to_tree` + `git_diff_print` → `common_print_cb` → `lwsac_use` per line, whole diff accumulated before any output), same-shape accumulations: `lib/job/tree.c:70` (per-entry lwsac for a dir listing), `lib/job/blame.c:226` (per-hunk-commit bhi + strings)
- Reachability: repo pusher commits a change whose diff is huge (e.g. add a ~500MB text file — binary files collapse to a one-line "Binary files differ", so the vector needs text); then any unauthenticated `GET /git/<repo>/commit/<oid>` or `/patch/<oid>` (or the pusher himself) runs the full `git_diff_print` into the ctx lwsac in a single unyielded `job_commit_start` slice (worker pinned for the duration too — F-010 family).
- Preconditions / gating: default build; repo-content-gated (pusher), unauth-detonatable.
- Attack path: craft + push the bomb commit → request the commit view → lwsac grows ~diff-size (4-byte prefix + content per line, lwsac chunk overhead) → 2–3 concurrent views push the process against its LimitAS=1500M (shipped systemd unit) → allocation failures daemon-wide or OOM kill (Restart=on-failure recovers, so degradation/DoS).  Tree listings of crafted multi-million-entry dirs and blame on pathological histories accumulate the same way at smaller scale.
- Evidence: no size cap anywhere on the lwsac growth in these paths; `job_commit_start` runs to completion before the streaming loops begin; libgit2 emits text diffs line-by-line with no bound.
- Impact assessment: practical memory-pressure DoS on the default deployment, gated on repo content (pusher).  Medium, medium confidence (mechanism code-verified; AS cap + graceful lwsac-NULL failure bound the worst case at process-wide allocation stress rather than silent corruption).
- Remediation sketch: cap the accumulated diff (stop collecting past e.g. 10MB and emit a truncation marker, or stream the diff via the libgit2 callback directly into the output buffer instead of staging it); bound tree-listing lwsac by entry count; that also removes the single-slice pinning.

## Checked clean

1. **commit.c lac-walk arithmetic** — `common_print_cb` allocs are exact
   (4 + origin + content = len+5); the record walk's `*p++` prefix read
   advances the payload pointer; `ofs`/`pos`/`size` accounting consistent
   incl. the first-chunk `lwsac_sizeof(1)` header difference (verified
   against lwsac.c) and `lwsac_align` at record boundaries; the NUL-hit
   bail truncates gracefully.
2. **Purify discipline** — every repo-content emission in all four files
   goes through `ellipsis_purify`/`jg2_json_purify` (commit body, diff
   text, blame signatures via signature_json + summaries + orig_path,
   tree names, bloblink composition); raw content only in `plain` mode
   (served `text/plain` + nosniff) and raw `patch` mode (text/plain) —
   correct by content-type.
3. **Streaming bounds** — plain/blob memcpy and purify caps respect
   `end - p` with the reserve seals (m/6 purify cap in tree's blob path,
   84×6 vs the 768/512 reserves in commit); `return 0` on space
   exhaustion resumes via partway.
4. **libgit2 lifetimes** — commit/tree start paths free exactly once on
   every goto-chain (bail→bail1→bail2 fallthrough verified); blame
   serializes signatures/summaries into its own lwsac copies before the
   blame object is freed; blob frees pair with rawcontent use; plain
   streams from the mmap without copying (memory-cheap by design).
5. **Path handling** — `git_tree_entry_bypath` lookups cannot traverse
   (git tree names cannot contain '/'; crafted `..` entries only match
   same-named entries, staying inside the tree object graph — display
   only); `inline_filename` composition is bounded and built from actual
   tree names.

## Notes (not findings)

- `blame.c:292-293` — the same-signature list insertion is a no-op
  (`b->next_same_fsig = bhi->next_same_fsig` — clearly meant
  `b->next_same_fsig = bhi`); the list has no consumer, so cosmetic.
- `blame.c:175-189` — if the final-commit lookup fails, the already-looked-up
  orig commit leaks (error-path only, broken-repo-object gated).
- `blame.c:241` — the copied `bhi->hunk` keeps interior libgit2 pointers
  (orig/final_signature) that dangle after `git_blame_free`; never
  dereferenced (the fixed-up copies are used) — latent trap worth a
  comment.
- `tree.c:277` — `strrchr(p + strlen(p) - 1, '/')` scans from the *last*
  character, so the basename extraction never strips the directory part
  (`blobname` shows the full path); cosmetic.
- `tree.c:432` — `head = lp_to_te(ctx->sorted_head, ...)` executes even
  after the blob path destroyed the list (container-of-NULL compute,
  unused in that path); harmless but ugly.
- `commit.c:269` — dead `if (!ctx->body)` inside a loop already guarded
  by it.
- blame/commit single-call compute (git_blame_file, git_diff_print) pins
  a worker until libgit2 returns — the shed bounds admission for blame;
  commit/patch URLs are not in the shed set (see F-016's slice remark).

## Coverage

Full deep-read: `lib/job/blame.c` (540), `lib/job/commit.c` (376),
`lib/job/tree.c` (503), `lib/job/plain.c` (81); witness reads: lws
lwsac.c (sizeof semantics), blob_from_commit left to lib-util's pass
(recheck added).  Both units → `done`.

## Methodology notes

- None.
