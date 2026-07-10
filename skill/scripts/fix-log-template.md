# Phase Fix Log Template

> Copy to `plans/phase-<ID>-fix-log.md` at the START of each executable phase; fill it as you go.
> This is the single highest-leverage anti-churn habit in a long port: it stops the team (and future
> you) from re-litigating refuted hypotheses and from trusting findings that have gone stale.
> One file per phase. Keep `PS2_PROJECT_STATE.md` as the rolling summary; keep narrative detail here.

## Phase
- Phase / ID:
- Date:
- Goal (one sentence):
- Starting baseline (what worked before this phase, with the metric):

## Root Cause / Finding
> Evidence, not symptom. Separate what you OBSERVED from what you CONCLUDED.
- Runtime evidence (probe output, capture metric, crash PC + actual `a0`/`sp`/`ra`):
- Reference evidence (PCSX2 register/memory, `.gs` dump, PCSX2 source citation, static-export decompile):
- Which LAYER (recompiler codegen / runtime / game logic / host)? Why that layer and not the others:

## Rejected Hypotheses  ⟵ DO NOT SKIP
> Every theory you disproved, WITH the proof. This is what stops the next phase re-chasing it.
| Hypothesis | How it was refuted (evidence) |
|------------|-------------------------------|
|            |                               |

## Changes
- Change 1 (file, what, why it addresses the ROOT):
- Diagnostic levers added/removed (env flag, default on/off, kill-switch name):

## Build & Verify
- Exact build command(s) + result (exit code, warnings):
- Verification run + result (metric before → after):
- Marker confirmed in binary (`grep -c <marker> <exe>`):
- Regression checks (prior milestones still pass?):

## Validity / Stale-When  ⟵ DO NOT SKIP
> A finding is only valid for the build state it was measured on.
- This finding holds ONLY while: <condition / which other fixes are on/off>
- RE-VERIFY this if any of these change: <paths/fixes that would invalidate it>

## Remaining Blocker / Next
- Current blocker + evidence:
- Next proposed phase:

## Do-NOT-Carry-Forward
> One-line bullets the next phase must not re-open (with the phase that settled each).
-
