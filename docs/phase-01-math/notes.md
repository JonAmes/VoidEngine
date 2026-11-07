# Phase 1 — notes

## Decisions & findings
- **Decision #34 (user, 2026-11-07):** world frame = right-handed, Y-up, −Z forward. Consequences recorded in PLAN.md decision #34: GLB pass-through is identity (assert the glTF Y-up invariant instead of rotating), OpenXR path needs no basis transform, euler convention = yaw(Y) → pitch(X) → roll(Z) right-handed.
- Epsilon semantics for `approximately_equal`: absolute eps below |x|≈1, relative eps beyond — documented at 1.1, every comparison test names which regime it exercises.

## Gotchas
(to fill as tasks land)
