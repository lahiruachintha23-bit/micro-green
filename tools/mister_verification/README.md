# Mister safety verification

A host-side harness for one property of the firmware:

> once commands stop arriving, the mister relay must reach OFF within a bounded time.

It exists because the ESP32 toolchain can't check this. A compile proves the code
builds; it says nothing about whether a state machine can latch a relay on.

## Run it

```sh
g++ -std=c++17 -O1 -Wall -o verify mister_test.cpp && ./verify
```

Expected:

```
scenarios checked : 46800
NEW violations    : 0
OLD violations    : 15500  (expected > 0)
PASS
```

## Why `mister_sim.cpp` contains the old logic too

`OLD violations` must stay above zero. It's the control: a harness that passes on
both the buggy and fixed versions isn't testing anything. If you refactor the
mister code and this number drops to zero, the test has gone blind — fix the test
before trusting it.

## What it covers

Every command sequence up to length 4 drawn from eight command types (cloud
spray / off / auto, local spray, local mode changes, mode set to ManualOn /
ManualOff / Auto), crossed with both germination states and five times of day.
46,800 scenarios. `loop()` is stepped at 50 ms; `millis()` starts non-zero so the
harness doesn't accidentally depend on time starting at 0.

Worst-case relay-on is `MISTER_SPRAY_DURATION + MISTER_MAX_CONTINUOUS_MS`
(310.5 s) — a timed one-shot expiring and chaining into a continuous spray, which
the continuous ceiling then terminates by forcing the mode to `ManualOff`. That
ceiling is the intended bound, so lower `MISTER_MAX_CONTINUOUS_MS` if you want a
tighter guarantee.

## Keeping it honest

`mister_sim.cpp` is a hand transcription of the mister branches in
`src/main.cpp`, not the firmware itself. It drifts if you edit one and not the
other. When you change mister logic in `main.cpp`, mirror it here — or delete this
directory rather than leave a harness that vouches for code it no longer matches.
