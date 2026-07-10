# Upstream PR Adoption Log — Template

> Copy to `plans/pr-adoption-<PR#>.md` BEFORE touching any upstream change. Governing rules:
> `10-agent-guardrails.md` §2 "Adopting an upstream fix onto a customized fork" — NEVER
> `git merge`/cherry-pick onto a diverged runtime; port hunks surgically. This log is what lets a
> future rebase know exactly what was taken, adapted, or refused — and why.

## PR
- Upstream PR / commit: <!-- link + title -->
- What it claims to fix/add (one sentence):
- Author's own caveats ("missing X", "basic", "WIP"): <!-- verbatim — incomplete upstream work
     is NOT adopted just because it "would likely be neutral" -->
- Why we want it (which LOCAL symptom/blocker it maps to):

## Divergence Assessment (per touched file)
<!-- Measure BEFORE deciding. A 0-line-diverged file = clean apply candidate;
     a heavily-diverged core file = port the IDEA by hand, never the diff. -->
| File touched by PR | Our local divergence from PR base (lines / summary) | Verdict |
|--------------------|-----------------------------------------------------|---------|
|                    |                                                     | apply clean / hand-port idea / skip |

## Hunk Decisions
<!-- One row per logical hunk/idea. "Smallest correct form" rule: prefer the lightweight
     version (e.g. post-wake yield) over heavy machinery unless the symptom demands it.
     Skip hunks that pull new build deps or rewrite subsystems we already validated. -->
| Hunk / idea | Decision (applied / adapted / deferred / refused) | Form we actually used | Why |
|-------------|---------------------------------------------------|----------------------|-----|
|             |                                                   |                      |     |

## Verification (per applied hunk)
- Build result (exit 0, marker in exe):
- A/B vs golden baselines (which harness, metric before → after):
- Regression sweep (prior milestones still pass?):

## Outcome
- Net state: <!-- e.g. "2 hunks adapted, 3 deferred until IOP phase, 1 refused (clobbers G5 swizzle fix)" -->
- Removal/revisit condition: <!-- e.g. "revisit deferred hunks when adopting real IOP scheduler" -->
- State-file entries updated: <!-- Known Upstream Issues / Learned Patterns rows touched -->
