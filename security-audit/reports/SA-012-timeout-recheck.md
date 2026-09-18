# SA-012 — idle-Track A + F-010 follow-up resolution (wsi timeout coupling) — 2026-09-16 — 2d61554

Selection: Track A empty (HEAD = 2d61554, every unit recorded there);
Track B exhausted (ledger complete since SA-011).  Work item taken from
the sweep-bookkeeping recheck queue: the F-010 follow-up ("the
build-then-answer flow ties trie builds to the requesting wsi's 60s
PENDING_TIMEOUT_THREADPOOL_TASK — needs runtime confirmation") —
resolved at **code level** this pass.  Working tree clean.

## Resolution: the 60s cap is real and broader than suspected

Verified facts (both sides code-read):

1. **lws timeouts are one-shot suls**: `__lws_set_timeout`
   (lib/core-net/wsi-timeout.c:240-253) inserts `wsi->sul_timeout` once
   for the given duration; `lws_sul_wsitimeout_cb` then closes the wsi.
   Nothing re-arms it implicitly — only an explicit `lws_set_timeout()`
   re-inserts the sul; the role/TX paths do not touch it on progress.
2. **gitohashi arms it exactly once per response**:
   `src/protocol_gitohashi.c:735` (`PENDING_TIMEOUT_THREADPOOL_TASK`,
   60s, after `http_reply`) — the chunk-write path (:748-762) never
   re-arms.  (The 30s `PENDING_TIMEOUT_THREADPOOL` at :654 covers only
   the pre-header queue wait.)

Consequences:

- **Pre-existing (all threadpool responses)**: any response whose
  generation + streaming exceeds 60s wall clock — huge blame, giant
  plain/tree views, slow cache reads — is truncated mid-body when the
  sul fires; the task sees STOPPING at its next slice boundary and
  aborts cleanly (slices/STOPPING discipline holds; no hang, no pool
  jam — the shed plus prompt STOPPING keep the service alive).
- **Fix-specific (the F-010 regression suspicion — confirmed)**: the
  build-then-answer flow (29a5557) ties trie completion to the
  requesting wsi; builds >60s can never finish (the pre-fix outlive
  flow could complete them after the stub response).  Huge repos are
  permanently unsearchable, retrying the full build per request with
  one temp trie file per attempt (disk bounded by the diskcache trim
  which counts the temps; the wasted rebuild work is bounded by the
  search-family shed).

Filed as **F-023**; the F-010 Status history gains the resolved
follow-up note.  The remediation is small: re-arm the timeout in the
chunk-write path (`lws_set_timeout(wsi, PENDING_TIMEOUT_THREADPOOL_TASK,
60)` after each successful `lws_write`), which converts the cap to a
60s-inactivity bound for streaming views, plus either a longer/armed
budget for the index-build phase or a return to a decoupled builder
for the trie case.

## Findings

### F-023 [Low/medium] Threadpool responses hard-capped at 60s wall clock — long views truncated mid-stream; huge-repo trie builds can never complete (retry + temp litter per attempt)
- Location: `src/protocol_gitohashi.c:735` (single arming, never re-armed) + lws one-shot sul semantics (`lib/core-net/wsi-timeout.c:240-253` — only explicit `lws_set_timeout` re-inserts)
- Reachability: content/size-gated — repos whose views take >60s (huge blamable files, giant listings) or whose trie index build exceeds 60s.
- Attack path: unauthenticated request for a huge view → headers sent → 60s sul fires mid-generation → wsi closed → task STOPPED at next slice → truncated body (or, for index builds: aborted build, one `~temp` left in the cache dir, full retry on the next request — the repo stays unsearchable forever).
- Impact: availability/functional truncation, content-gated; no memory-safety or pool-liveness impact (STOPPING is honored per slice).  Low, medium confidence on real-world frequency (mechanism code-verified both sides).
- Remediation sketch: re-arm the timeout after each successful chunk write (making it an inactivity cap); for the search-build phase, arm a larger explicit budget or decouple trie completion from the requesting wsi again (a completion marker the next requester picks up).

## Checked clean

- The F-010 slice/STOPPING discipline is unaffected by the cap — the
  abort is clean at slice boundaries (verified reasoning against
  task_function's entry check).
- Temp-trie litter from aborted builds is disk-bounded by the JSON
  cache's trim walker (counts all regular files — the F-003/SA-003
  nuance), so the retry loop cannot exhaust disk, only waste worker
  time (shed-bounded).

## Coverage

No unit re-audited (no deltas); lws wsi-timeout.c + threadpool
interaction read as witnesses; protocol_gitohashi.c timeout sites
re-checked in current shape.  Ledger unchanged (all units done at
2d61554).  Next free: SA-013 / F-024.

## Methodology notes

- This pass selected its work from the ledger's standing-recheck queue
  (Track A empty, Track B exhausted) — consistent with the
  methodology's intent that no unit is lost and bookkeeping items get
  resolved; the queue is now empty except the F-009 theoretical
  residual and the F-007 hardening shelf (both accepted/as-is).
