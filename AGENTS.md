# AGENTS.md — Mixxx Project Instructions

Read [README.md](README.md) for an overview and
[CONTRIBUTING.md](CONTRIBUTING.md) for build instructions, coding conventions,
pre-commit setup, Git workflow, and pull-request guidance.

<!-- Start of AI-assisted policy restoration; original Mixxx policy reproduced verbatim. -->
## AI Agent Policy

> **Important:** The Mixxx project only accepts contributions driven by human developers.
> Automated AI agents acting autonomously on a user's behalf are not welcome and may result
> in pull requests being closed without comment.
> But using AI tools that **assist** the human contributor with code generation, refactoring,
> documentation and reviewing the work before submission is welcome.

### No Autonomous Pull Requests

AI agents **must not** open, update, or re-open pull requests autonomously on behalf of a user.
All pull requests must be explicitly reviewed, approved, and submitted by the human developer
themselves. An agent may prepare and stage changes locally, but `git commit`, `git push`, and PR
creation must be deliberate human actions. Before this, a test of the code change by a human,
using a real DJ setup, is required.

### No Automated Responses to PR Review Comments

AI agents **must not** post replies to review comments, questions, or change requests left on
a pull request or its commits. All communication in the PR thread must come directly from the human contributor.

### AI Code-Reviews

AI agents **may** and **should** perform code reviews on explicitly request in scope of the PR.

### No Autonomous Creation of Issues, Bug Reports or Feature Requests

AI agents **must not** open, update, or re-open Issues, Bug Reports or Feature Requests autonomously
or on behalf of a user.
All Issues, Bug Reports, and Feature Requests must be explicitly reviewed, approved, and submitted by
the human user themselves.

### AI-Generated Text Must Carry a Disclaimer

Any text (PR description, commit message body, code comments, documentation) that was written
autonomously by an AI Agent **must** be framed with an disclaimer that the text is autonomously generated
by the AI Agent, at both the start and the end of that text block.
<!-- End of AI-assisted policy restoration. -->

## Development workflow

- Work on a focused branch for each feature or bug fix.
- Install and use the repository hooks:
  `pre-commit install` and `pre-commit install -t pre-push`.
- Follow the existing conventions and `.clang-format`. Format only new or
  modified code; keep formatting-only changes separate from logic changes.
- Keep commits small and buildable. Run the relevant tests (`ctest` or
  `mixxx-test`) and pre-commit checks before handing changes off.
- Keep commits and pull requests focused. Do not include unrelated cleanup,
  generated files, IDE files, or other untracked artifacts.
- Write commit messages in the imperative mood, wrapped at 72 columns, and
  explain what changed and the reasoning behind why. Describe the change, not the tool used to make it.
- For GUI changes, include before-and-after screenshots when preparing a pull
  request. For audio or hardware behavior, test with the relevant real setup
  before submission.
- Use distrobox's mixxx-build to build and run mixxx, you have full authorization and dominion within that distrobox and this folder.

## Upstream collaboration

- Search existing issues, pull requests, documentation, and discussions before
  starting a new upstream conversation. Prefer one clear, well-supported report
  over several speculative or duplicate ones.
- Keep upstream interactions deliberate and low-noise: do not send duplicate
  reports, repeated status messages, or unsolicited review replies.
- A human contributor reviews the diff, validates it, and decides what to submit
  upstream. Prepare changes locally unless the human has explicitly asked for an
  upstream action.
- If contributing to someone else's open pull request, target their fork and
  link it from the upstream pull request so the work stays discoverable.
- Never force-push to an upstream Mixxx repository.

## Key architecture

- **ControlObject/ControlProxy**: `[Group], key_name` inter-component
  communication.
- **Engine thread**: real-time audio;
no allocations or locks.It may emit Qt signals but cannot receive them.- **`parented_ptr`/`make_parented`** : Qt object - tree ownership.An object must have a parent before `parented_ptr` is destroyed.

                                                                                                                          ##Project layout

```text src / C++ source(engine /, controllers /, library /, mixer /, effects /, qml /, preferences /, util /, test /) res
                        / Resources(controllers / JS / XML, skins /, qml /) cmake / CMake modules tools / Python helper scripts
```

<!-- Start of AI-assisted fork coordination note. -->
For fork delivery priorities, consult the [canonical workflow](https://github.com/0cwa/mixxx/blob/mixxx/main/.github/FORK_AGENT_WORKFLOW.md) on the fork's default branch. Do not assume this branch is ready to merge; dependent PRs must be checked for inherited policy changes.
<!-- End of AI-assisted fork coordination note. -->
