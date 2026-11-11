# Workflow: how we split up work (keeps usage low, mistakes few)

Why: every message in a session re-reads everything loaded so far, so long sessions and big always-loaded docs cost a lot.

## Rules
1. **One task per session.** Finish it, commit, then `/clear` (or start a new session) for the next one.
2. **Plan first, build second.** Ask for the plan only; answer the questions; then say "go". Park anything that isn't part of this task on the Roadmap in `CLAUDE.md`.
3. **Mac tests before flashing.** `make -C test` first; flash only when the feature is complete.
4. **Give specific hardware feedback** ("reverse clicks at the bar line"), not just "needs work".
5. **Commit after each working step**, so a bad step is one revert.

## Cleanup rule
The size limits and the cleanup steps are in `CLAUDE.md` ("Cleanup rule"). Claude checks them at session start; you can also say "run the cleanup check" any time.

## Where each update goes (one place each)
| What changed | Update |
|---|---|
| Every code change | `src/version.h` + ONE line in `docs/CHANGELOG.md` |
| A control, LED or log message | `docs/HARDWARE_MAP.md` |
| Looper / beat repeat / slice / pitch behaviour | `docs/DESIGN_LOOPER.md` (current rules only, no "before 0.x" history) |
| Hardware test result | `docs/HARDWARE_TEST.md` (fold passed items into "Passed") |
| Controls/behaviour summary, new roadmap item | `CLAUDE.md` (short; no per-version history) |
| Why / details of a change | the git commit message |

## Task sizes that work well
Small: one control or one fix. Medium: one new feature with its Mac test. Split anything bigger into a plan session and one build session per piece.
