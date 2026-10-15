> **AI-assisted administrative snapshot; human review required before merging.**

# Fork PR triage — 2026-10-15

A **dated decision ledger** for 0cwa/mixxx, not an up-to-date PR API report. Before acting, confirm each PR's current branch, head SHA, tests, merged status, and downstream dependents. Scope is project management and low-risk administration, **not** a feature implementation plan.

## Current administrative checkpoint (October 15)

**State: 49 open PRs (27 draft / 22 non-draft).** This is a point-in-time snapshot, not a live query. The active priorities are in [FORK_AGENT_WORKFLOW.md](FORK_AGENT_WORKFLOW.md); P0 audio reliability and exact-head tests outrank new features.

- **Merged:** [#94](https://github.com/0cwa/mixxx/pull/94) landed the canonical agent workflow and preserved the original human-contribution policy on `mixxx/main`. It is no longer a draft.
- **Policy restored on parent heads:** [#23](https://github.com/0cwa/mixxx/pull/23) at `11b35fe68583a9d086063153cda1cb62f765c67f` and [#64](https://github.com/0cwa/mixxx/pull/64) at `e21c672db8bc3ee54b038e0f4469f15874056b5a`. These are **documentation-only new heads**, not proof of successful runtime validation. #64 remains a CI-only checkpoint and must not be merged.
- **Policy not yet inherited in direct children:** #31 (child of #23), and #65, #68, #69, #71, #72, #73, #74, #76, #77, #78, #80 (children of #64). Their PR descriptions flag the policy gap. Do **not** land or retarget them until the policy survives the parent/child diff. Avoid mass rewriting all child heads just to clear documentation; instead synchronize at the next selected integration and retest the exact resulting head.
- **Flatpak:** [#54](https://github.com/0cwa/mixxx/pull/54) now has a single net manifest option, `-DSIGNALSMITH=ON`, preserving upstream #16515's `run-tests: true` and `BUILD_TESTING=ON`. [#57](https://github.com/0cwa/mixxx/pull/57) was closed as superseded without merging. [#69](https://github.com/0cwa/mixxx/pull/69) remains open against #64, whose Flatpak tests are still disabled; its distinct patch must be integrated after checking downstream effects and CI. No Flatpak runtime tests were run by this administrative pass.
- **Duplicates:** #25 was closed in favor of commit-descendant #26. #35 remains the narrow Rekordbox key-sort candidate; #21 remains draft until its distinct AudioUnit changes are isolated. #80 remains the cumulative Media Foundation candidate; older MF PRs are not safe to retire until upstream equivalence and Windows-specific validation are demonstrated.
- **Next actions:** First complete exact-head #23 validation for [#93](https://github.com/0cwa/mixxx/issues/93) and investigate Linux dropout [#81](https://github.com/0cwa/mixxx/issues/81). Then integrate the selected #69 Flatpak change into the CI composition, preserving enabled tests and Signalsmith, and synchronize policy for the particular downstream branches actually advancing to review. Avoid another sweep of speculative PR creation.

## Actions already taken in the fork

- Closed [#45](https://github.com/0cwa/mixxx/pull/45) *without merge*, because disabling Flatpak tests opposes upstream [#16515](https://github.com/mixxxdj/mixxx/pull/16515), which already enables those tests. Downstream compositions may still include the disabling patch.
- Converted [#37](https://github.com/0cwa/mixxx/pull/37) to draft: its AGENTS.md replacement removes the upstream human-review/AI policy. [#79](https://github.com/0cwa/mixxx/pull/79) only fixes formatting on the same policy-stripped text.
- Converted [#23](https://github.com/0cwa/mixxx/pull/23), [#21](https://github.com/0cwa/mixxx/pull/21), [#25](https://github.com/0cwa/mixxx/pull/25) and [#31](https://github.com/0cwa/mixxx/pull/31) to draft pending specific integration, scope, successor or runtime evidence.
- Added 30 topical labels (9 on PRs and 21 on 12 follow-up issues); all remaining 53 open PRs had topic labels after that pass.

## Decisions, without premature closures

| Item | Keep / consolidate / close | Next administrative action |
| --- | --- | --- |
| #37, #79 | Draft / superseded candidate | Preserve original AGENTS policy. Compare this new workflow against their unique useful text; do not merge policy-stripped AGENTS |
| #54, #57, #69 | Reconcile, **not** close yet | #54 adds SIGNALSMITH while disabling Flatpak tests; #57/#69 restore upstream default tests on different bases. Verify final canonical branch state before retiring patches |
| #21, #35 | Consolidate pending evidence | Prefer narrow #35; #21 currently also changes AudioUnit/effects. Verify unique useful fix before closing #21 |
| #25, #26 | Consolidate pending evidence | #26 appears corrected successor; compare full changes/tests before retiring #25 |
| #56, #60, #63, #70, #80 | Consolidate later | #80 cumulative candidate; compare upstream #17151 and perform Windows runtime tests before retiring ancestors |
| #47, #68, #77 | Preserve separate test value | Integrate fallback/serialization assertions once, on selected downbeat model |
| #32 | Coordinate, don't assume upstream completion | The overlapping upstream effects-manifest PR #16176 remains open |
| #23 | Keep **draft**, integration/test only | Issue #93 requires CI/ASan evidence on the exact current head |
| #64 | **Never merge as-is** | Fork CI validation checkpoint explicitly not intended for merge |
| #31 | Keep draft | Upstream stem work and DJ/runtime test still unresolved |

## Canonical workstream map (fork open PRs at the original 53-PR snapshot)

- **Integration/testing:** #23, #64
- **Documentation/agent direction:** #37, #79
- **Packaging/build/developer tooling:** #36, #38, #39, #40, #51, #54, #57, #69
- **Bungee/engine/caching diagnostics:** #28, #41, #42, #49, #50, #52, #73, #74, #78
- **Controllers/vinyl control:** #27, #34
- **Rekordbox:** #21, #25, #26, #35, #46, #55, #59, #65, #66, #67, #75, #76
- **Waveforms/memory/beatgrid:** #43, #44, #47, #48, #53, #58, #68, #71, #72, #77
- **Media Foundation:** #56, #60, #63, #70, #80
- **Effects/QML:** #32, #33
- **Stem conversion:** #31

## Subsequent documentation cleanup (same date)

- Created [draft documentation PR #94](https://github.com/0cwa/mixxx/pull/94) from the fork's upstream-tracking default branch. It adds this dated ledger and the fork-specific delivery workflow and **retains every original AI/human-contribution rule** in `AGENTS.md`.
- Closed [#37](https://github.com/0cwa/mixxx/pull/37) and [#79](https://github.com/0cwa/mixxx/pull/79) **without merging**, as their policy-stripped `AGENTS.md` edits are superseded by the safer doc-only PR #94. Their commit history and branch content are preserved.
- **Unresolved inheritance warning:** older composition and forest branches, including PR #64's integration branch, may **still contain** the removed upstream policy. The closure of #37/#79 and creation of #94 do not amend those branches. Before choosing any integration head for delivery, restore/verify its `AGENTS.md` human-contribution policy and recheck the diff.
- Verified that PR #57's Flatpak restore diff removes `-DSIGNALSMITH=ON` while PR #69's alternative restore keeps it; do not treat them as identical changes or blindly close the former without confirming intended separation.

## Verified additional PR consolidation and branch check

- [#25](https://github.com/0cwa/mixxx/pull/25) closed without merge after comparing exact heads: [#26](https://github.com/0cwa/mixxx/pull/26) is **2 commits ahead and 0 behind** #25's current recorded head. This proves commit ancestry, not current-head CI readiness. The #25 branch was not deleted.
- Checked `AGENTS.md` at the actual heads of **#23 and #64**: both omit the original Mixxx AI/human contribution policy. **#47 and #26** retain those sections. Restoring the policy in the chosen integration line remains a gating administrative task; PR #94 does not fix old branch heads.
- Added coordination-only PR-body notes to #23, #64, #26 and #54, without modifying their code, branch refs or merge status.

## Active priority gates

1. **CI correctness:** [issue #93](https://github.com/0cwa/mixxx/issues/93) — PR23's current Oct 10 head did not have an exact-head Actions run at audit time. The Oct 8 green revision is not proof of its descendant.
2. **DJ reliability:** [issue #81](https://github.com/0cwa/mixxx/issues/81) — real Linux audio silence remains unexplained; synthetic test passes do not resolve it.
3. **Administrative cleanup:** reconcile #54/#57/#69 Flatpak defaults and #37/#79 policy, then deduplicate Rekordbox PRs.

For each action, record: **confirmed target**, **unique changes retained**, **upstream state/date**, **exact tested SHA**, **blocker**, **next action/owner**. Never delete a branch simply because its PR closed.

> **End of AI-assisted administrative snapshot.**
