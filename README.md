# Football Arena

A third-person football (soccer) gameplay prototype built in **Unreal Engine 5.8** with C++ and Blueprints.

The goal of the project was to get the *feel* of arcade football right — ball weight, shot power, curl,
and close control — rather than to build a full match game. Everything is tuned against measured
in-editor physics rather than by eye.

---

## Gameplay Systems

### Ball physics
Custom rigid-body setup on a regulation 22 cm ball: tuned linear/angular damping, restitution and
friction, plus a per-tick **Magnus force** so struck balls curve in flight and an added air-drag term
so shots bleed speed realistically over distance.

### Shooting — 6 shot types
Shoot, Pass, Through Ball, Lob, and Finesse (left/right, bound to a shoulder-button modifier).
Each is hold-to-charge with its own response curve:

```
factor = floor + (1 - floor) * chargeAlpha ^ exponent
```

Every shot has its own `floor`, `exponent`, base power, lift and spin, so a tap and a full hold
produce genuinely different trajectories. Full charge deliberately overhits — the shot that scores
is the well-judged one, not the maxed one.

### Aim assist (C++)
`UFootballLibrary::GetAimAssistedDirection()` bends a roughly-aimed shot toward the goal:

- **Dot-product cone gate** — shots more than ~60° off target get no help at all.
- **Smooth-step falloff** across the edge of the cone, so a small aim wobble can't snap the shot.
- **Distance scaling** — more help from range, where a 1° error costs far more lateral accuracy.
- **Yaw only.** `ApplyKick` computes `normalize(normalize(dir) + lift * up)`, so any Z in the aim
  direction adds straight onto the tuned lift. The assist is flattened to the horizontal plane to
  keep shot trajectories intact.

### Dribbling
Close control implemented as a velocity controller rather than attachment: the ball's target velocity
is the player's own velocity (feedforward) plus a proportional pull toward a guide point ahead of the
feet, speed-clamped and interpolated. The ball stays at the feet through sprints and sharp turns
without ever being parented to the player.

### Goal detection and scoring
`AGoalTrigger` is a C++ actor with a `UBoxComponent` across the goal mouth. On ball overlap it
broadcasts a `BlueprintAssignable` dynamic multicast delegate, which the game mode binds to for
score increment and ball reset. A separate out-of-bounds volume resets the ball without scoring.

### HUD
A UMG power meter with segmented charge feedback, colour-ramped as the shot charges.

---

## C++ Source

| File | What it does |
|---|---|
| `Source/FootballArena/FootballLibrary.h/.cpp` | `UBlueprintFunctionLibrary` — aim assist direction and distance-based shot power |
| `Source/FootballArena/GoalTrigger.h/.cpp` | Goal-detection actor, box overlap to multicast delegate |

Everything else is Blueprint, kept there deliberately: per-frame vector math and delegates live in
C++, while data flow, UI and asset wiring stay in Blueprint where they are faster to iterate on.

---

## Project Layout

```
Config/                              Project settings, incl. custom "Ball" collision channel
Source/FootballArena/                C++ gameplay module
Content/Football/Blueprints/
    Ball/                            BP_Football - physics, Magnus force, ApplyKick
    Characters/                      BP_FootballPlayer - input, dribbling, all 6 shots
    GameManager/                     Game mode, scoring, out-of-bounds volume
    UI/                              WBP_PowerMeter
Content/Football/Maps/Arena.umap     Practice arena
Content/Football/Physics/            Physics materials (ball, grass)
Content/Football/Input/              Enhanced Input actions and mapping context
```

---

## Notes

- **Input is gamepad-based** (Enhanced Input). Run in a New Editor Window or Standalone —
  gamepad input does not route reliably to the "Selected Viewport" PIE mode.
- **Character meshes and animation clips are not included.** They are external Mixamo downloads and
  are not redistributed here, so a fresh clone will show the player without a mesh. All authored
  gameplay code, Blueprints, physics setup and the level are committed.
- Requires **Unreal Engine 5.8**. Right-click the `.uproject` and generate project files, then build
  the `FootballArena` module before opening.
