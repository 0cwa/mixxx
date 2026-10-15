> **AI-assisted fork-maintenance guidance; review by a human before merging.**

# Fork delivery workflow for 0cwa/mixxx

This applies to the **fork only**. It supplements rather than replaces the human-contribution, review, and AI-assistance rules in [AGENTS.md](../AGENTS.md) and [CONTRIBUTING.md](../CONTRIBUTING.md). It does not authorize autonomous upstream submissions, PR-thread responses, bug reports, commits or merges. The owner must explicitly direct and review repository changes. Actual DJ/audio hardware testing cannot be replaced by simulated CI tests.

## What counts as progress

Prioritize **working, independently reviewable and eventually landed outcomes** over number of commits, PRs, reviews or new branches.

Every active PR must have an identifiable:

- Outcome and user-visible acceptance criterion.
- Role: canonical implementation, unique follow-up, diagnostic, documentation, or disposable CI checkpoint.
- Target/base branch, full head SHA, dependencies and intended upstream destination.
- Upstream check: exact source/PR, date, and one of **already upstream**, **partially upstream**, **competing upstream PR**, **unique fork change**.
- Proof: tests that **actually ran** on the exact head SHA and platform, manual testing if needed, and remaining validation gaps.
- Owner, next action, and reason for being blocked or draft.

A green status from CodeRabbit when it **skipped** its review is not a substantive review. A previous green ancestor run does not validate a changed head, including a head with `[skip ci]`. CI compilation does not substitute for Windows-specific Media Foundation runtime tests, real audio callbacks or DJ-hardware validation.

## Branches and delivery roles

- `mixxx/main` tracks upstream: do not casually put feature work or mega-merges there.
- Fork [#23](https://github.com/0cwa/mixxx/pull/23) is an **integration and test vehicle**. It is not a substitute for focused human-reviewed PRs. Revalidate its exact head, tracked in [#93](https://github.com/0cwa/mixxx/issues/93).
- Fork [#64](https://github.com/0cwa/mixxx/pull/64) explicitly describes itself as a **non-mergeable-by-intent CI checkpoint**. Never treat it as a delivery PR by accident.
- Prefer one small **canonical implementation** for each feature/fix, and dependent follow-ups containing *only their unique delta*. Keep a simple dependency chain. Before retiring an ancestor, verify commits, tests and patches survive in the chosen descendant.
- Upstream [Bungee #16435](https://github.com/mixxxdj/mixxx/pull/16435) is an existing draft. Human contributors should advance the existing discussion rather than submit parallel equivalent work.
- For a documentation-only coordination task, do not change audio-engine behavior, run huge feature refactors, or force-push branches.

Limit active implementation initiatives to **three** unless a human explicitly changes priorities. Other drafts may be retained as archived candidates, with one concrete next action each.

## Mandatory upstream-first procedure

1. Before developing a proposed fix, inspect `mixxxdj/mixxx` `main` **and the relevant release branch**, e.g. `2.6`.
2. Search upstream open **and merged** PRs and issues by symbols, file paths and behavior. Compare actual patches; titles and issue numbers are insufficient.
3. Record exactly which behavior remains unsolved. For a competing open upstream PR, prefer a human-coordinated contribution to that work.
4. If already upstream, remove only redundant fork changes from the selected integration path *after* confirming unique tests/behavior are retained. Document the replacement and close the fork duplicate after human review.
5. If partly upstream, integrate the upstream baseline, retain just the genuine delta, and rerun tests on the resulting SHA.
6. Never open an upstream issue or PR, post to review discussions, or declare completion autonomously; follow the human contribution policy in `AGENTS.md`.

## Merge, closure, and review rules

- Each focused PR should have a finite acceptance gate and an explicit **ready/draft** state.
- For a superseding PR, name the replaced PR(s) and check unique source/tests, branch ancestry and downstream dependencies before closing anything.
- Closing a PR **does not remove** its code from descendant branches. Don't delete heads with dependents. Never assume that retargeting a stacked PR preserves its intended diff.
- Each merge or close is a deliberate human-approved action. Prefer a clear, documented *superseded / already upstream / abandoned* outcome to indefinite duplicate PRs.
- Run pre-commit/pre-push hooks for changed files where available. Changes involving live audio, GUI or controller mappings need relevant runtime, screenshot or real DJ setup evidence.
- Preserve the original Mixxx AI agent policy in `AGENTS.md`. Documentation additions must **supplement**, not erase, that policy. AI-generated documentation must carry the original policy's start/end disclosure.

## Current triage examples — recheck before changing code

| Workstream | Current canonical candidate | Important caveat |
| --- | --- | --- |
| Flatpak tests | Upstream [#16515](https://github.com/mixxxdj/mixxx/pull/16515), merged May 28, 2026 | Fork #45 was closed without merge; #54's disabling state still needs correcting before retiring #57/#69 |
| Audio stream gaps | Fork [#80](https://github.com/0cwa/mixxx/pull/80) | Upstream [#17151](https://github.com/mixxxdj/mixxx/pull/17151) is merged, but covers only part; native Windows validation required |
| Rekordbox key sorting | Fork [#35](https://github.com/0cwa/mixxx/pull/35) | Fork #21 contains related but unrelated AudioUnit changes; verify/keep only one copy |
| Rekordbox V1 import | Fork [#26](https://github.com/0cwa/mixxx/pull/26), provisional | Check it fully supersedes #25, including tests, before closing #25 |
| Beat/downbeat persistence | Fork #47 with focused follow-ups #68/#77 | Preserve unique serialization/fallback tests in one canonical chain |
| Effects manifests | Fork #32, coordination only | Upstream [#16176](https://github.com/mixxxdj/mixxx/pull/16176) is still open with an overlapping guard |
| Agent instructions | This file + original AGENTS.md | Fork #37 and formatting child #79 must not replace human-contribution rules |
| Stem conversion | Fork #31, draft | Compare against open upstream [#15891](https://github.com/mixxxdj/mixxx/pull/15891) and require actual runtime validation |

## Weekly 20-minute maintenance review

1. Reconcile new upstream merges and changes in branch divergence.
2. Review exact-head tests for the three active initiatives and new failures.
3. Find old drafts, orphaned stacked branches and duplicate file-level diffs.
4. Update the canonical/depends-on/supersedes ledger; retire only confirmed safe duplicates.
5. End with specific next actions and validation owners, not another wave of PRs.

For the dated starting inventory and housekeeping history, see [Fork triage snapshot (2026-10-15)](FORK_TRIAGE_2026-10-15.md). It is a **snapshot**, not a live claim about open PR counts.

> **End of AI-assisted fork-maintenance guidance.**
