# Implementation-plan review log

Scope: planning documents only. Baseline: `abae0e9`. Implementation, commits and publishing
are outside this review. Each supervisor starts without conversation history and independently
reads the current plan, design, requirements, scaffold and assignment.

## Round 1

Fresh supervisor: `plan_supervisor_round1`. Result: six defects and four improvements.
The planner verified and addressed all ten:

| Finding | Resolution |
|---|---|
| Duplicate outer envelope fields bypass fast split | Task 8 and design §6.2: exact data-object end scan and adversarial tests |
| Sync-count oracle fails on a terminal Unsynced interval | Task 15: state-aware identity and explicit terminal cases |
| Output lifecycle missing | Task 19: virtual destructor, checked flush/finish, sticky failures |
| REST work cannot be cancelled through its interface | Task 24: cancel_all and completion guards |
| Runtime REST fatal response cannot stop capture | Tasks 24/26/27: fatal callback, common shutdown and depth-400 test |
| Budget formula lacks the 6 MiB fallback | Task 26: explicit term; independently recomputed ceilings 84/429/353 MiB |
| Refresh liveness assumes unbounded depletion is preventable | Task 33: constrained no-exhaustion generator plus required exhaustion case |
| Duration origin does not cross the capture interface | Tasks 26/27: required start argument, validation/expired-duration tests |
| CSV numeric validation underspecified | Task 21: strict types, overflow, checked timestamp reconstruction and direct tests |
| Replay basename collisions overwrite output | Task 21: preflight rejection before output changes, canonical deduplication |

The inherited fourteen unfinished round-3 items were also reconciled with their owning tasks:
snapshot minification, logger-owned formatting and real-file allocation coverage, request kind,
explicit failing shell assertions, verifier library, consumer stats inputs, sanitizer container
options, task ordering, exact endpoint assertions, fixture lookup and missing behavior/counter
tests. Design/requirements wording was aligned with the plan where these changes affected it.

Additional planner checks corrected the pre-tag S5 gate, per-stream/receive-capacity metrics,
the mandatory payload benchmark wording and an accidental comment terminator in a test example.

## Round 2

Fresh supervisor: `plan_supervisor_round2`. Result: two defects and one improvement, verified
against pinned simdjson/Boost source and addressed:

- Task 8 provides a bounded JSON visitor; Task 9 validates ignored values in the same traversal.
  Tests reject invalid ignored scalars, escapes and nested containers.
- Task 25 caps flat_buffer at 1 MiB and shrinks according to capacity. Task 27 observes retained
  and transient capacities under fragmented reads through a library test observer.
- HttpResult and snapshot records preserve the original steady receive timestamp across ring
  retries, so snapshot latency includes its wait.

Planner checks added buffered-bridge liveness, non-vacuous coverage depletion/recovery,
row-view lifetime across CSV-buffer growth, unsigned counter formatting and bounded epoch-log
accounting with cross-symbol duplicate detection. Network syntax validation keeps invalid inner
JSON out of CSVs; REST reuses the same 6 MiB workspace. The intentional extra network pass is
stated and benchmarked. Static plan/link/budget checks and the shell regression passed.

## Round 3

Fresh supervisor: `plan_supervisor_round3`. Result: two defects and one improvement, addressed:

- Task 36 owns the mandatory comments-only `.env.example` and checks that it is tracked.
- Tasks 13/14 expose actual full/partial comparison counts; Tasks 28/32 require successful
  synchronization, emitted rows and positive comparison evidence. Zero mismatches alone cannot
  pass live acceptance. Design observability and acceptance rules now match these gates.
- Task 7 uses a checked build pipeline and a conditional warning assertion; a clean grep result
  no longer fails a warning-free build.

Planner checks also specify event-domain validation before book mutation and keep missing or
unreadable replay inputs as per-file failures so other valid inputs can still complete.

## Round 4

Fresh supervisor: `plan_supervisor_round4`. Result: **no actionable issues or improvements**.
It independently reviewed the complete current plan/design/requirements, README, scaffold,
CMake/presets, CI, configuration and all six assignment pages without earlier review context.
Coverage included sequencing, finite-depth coverage, loss recovery, replay determinism,
ownership, memory bounds, test oracles, build wiring and submission gates. Remaining API
questions were checked against pinned simdjson 4.6.11 and Beast 1.74 sources.

Final planner validation: 37 ordered tasks, balanced code fences, valid local document links,
consistent live-gate counter names, memory ceilings 84/429/353 MiB and `git diff --check`.
Synthetic shell checks accept clean logs, reject warning logs, retain upstream build failures
and reject socket traces. These checks do not constitute a C++ build or live capture.

Status: review loop complete. No unresolved actionable planning findings. Only planning
documents changed; no implementation, builds, captures, commits or pushes were performed.
