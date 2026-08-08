# Remote access plan: Netlify + Firebase RTDB

Goal: open the dashboard from any network, see live sensor data, and control the pump.

## What already exists

More of this is built than it looks. The architecture is already the right one:

- `src/main.cpp` pushes telemetry to RTDB (`/sensorReadings`, `/deviceStatus`) and polls `/commands` every 5 s.
- `netlify/functions/motor-control.cjs` queues commands into `/commands` and reads back the latest reading.
- `data/index.html` is a complete dashboard; `netlify.toml` already publishes `data/`.
- `database.rules.json`, `firebase.json`, `.firebaserc` exist.

The ESP32 only ever makes **outbound** HTTPS calls, so it works from behind home NAT with no port forwarding, no ngrok, no DDNS. Keep that property.

```
Browser (Netlify HTTPS, anywhere)
        |  read /live, write /commands
        v
Firebase Realtime Database
        ^
        |  PUT /live, POST /history, stream /commands  (outbound only)
Ganesh ESP32 on home WiFi
```

## Why it doesn't work today

Four concrete blockers, in order of impact.

**1. Timestamp units don't match — this is the one making it look dead.**
Firmware writes `millis()/1000` (seconds of uptime). `motor-control.cjs` checks
`Date.now() - lastReading.timestamp < 60000` (epoch milliseconds). An uptime value
is ~1.7 trillion ms smaller than epoch now, so the gap is always huge and the
dashboard permanently reports `CLOUD ONLY (No Device)` even while the board is
pushing fine. Uptime also resets to 0 on every reboot, so charts are meaningless.
Fix: NTP via `configTime()`, publish real epoch ms.

**2. The dashboard never actually reads Firebase.**
`readFirebaseLatest()` is defined at `data/index.html:966` and never called.
`updateStatus()` only polls the Netlify function every 3 s. So it isn't realtime,
it burns function invocations, and it adds a hop of latency.

**3. `FIREBASE_DATABASE_URL` is probably unset in Netlify.**
Both functions read `process.env.FIREBASE_DATABASE_URL`. Unset means every call
throws and the UI shows disconnected regardless of the device.

**4. Project ID mismatch.**
`.firebaserc` says `microgreen-tray`. Everything else — `data/firebase-config.js`,
`FIREBASE_DB_BASE` in `main.cpp` — says `microgreen-system`. Deploying rules would
target the wrong project or fail.

## Data shape

Split hot state from history. Right now everything is `POST` (append), so
`/sensorReadings` and `/deviceStatus` grow without bound and nothing is pruned.

```
/live            <- PUT (overwrite) every 2-3 s. One small node. Dashboard subscribes here.
    temperature, humidity, soil, flow, height, stage,
    pumpMode, pumpState, motorStatus, fansActive, misterActive,
    floatSwitch, daysSinceGermination, deviceIp, ts
/history/<pushId>  <- POST every 5 min, for charts. Pruned to ~10 days.
/commands/<pushId> <- browser writes; device executes then DELETEs
/deviceStatus      <- PUT (not POST) so it stays one node
```

`/deviceStatus` currently uses `POST`, creating a fresh push key every 30 s
forever. Change to `PUT`.

## Milestones

Staged so you have a working remote dashboard at the end of M3, before any of the
larger firmware refactor in M4.

### M0 — Fix the mister latch first (do this before exposing controls) — DONE

While tracing the control path I found a bug that matters specifically *because*
you're about to make these buttons reachable from the internet.

`executeCloudCommand("mister_spray")` set `misterMode = "ManualOn"` **and**
`misterSprayInProgress = true`. That combination matched none of the four mister
branches in `loop()`, and the 10-second auto-off lived *inside* the `Auto` branch,
so it never ran either. **A "Spray Now" pressed from the cloud dashboard turned
the mister on and never turned it off** — it ran until someone sent `mister_off`.
Over an unreliable mobile connection, that's a flooded tray.

- [x] Hoisted the `MISTER_SPRAY_DURATION` timeout out of the `Auto` branch. It now
      runs unconditionally, before any mode branch is considered.
- [x] Added a `misterSprayTimed` flag so `mister_spray` is a one-shot that
      auto-expires, instead of latching `misterMode = "ManualOn"`.
- [x] Added `startMisterOneShot()` / `stopMisterNow()`. Every relay-off path now
      funnels through `stopMisterNow()`, so the flags can't drift out of sync.
- [x] Added `MISTER_MAX_CONTINUOUS_MS` (5 min) as a ceiling on continuous manual
      misting, since "Manual On" otherwise had no deadline at all. When it trips it
      forces `misterMode = "ManualOff"` — without that, the ManualOn branch would
      switch the relay straight back on the next time through `loop()`.
- [x] Fixed the local `/mister?action=spray` handler, which set no mode at all and
      so got cancelled immediately by the final `Auto` branch once germination ended.
- [x] `data/index.html` said "Spray 5s" while the firmware sprays for 10 s.

Post-fix invariant: the relay is driven LOW in exactly two places, and both stamp
`misterSprayStartTime` and arm a bounded stop.

**How this was verified.** There's no ESP32 toolchain in this environment and no
network to fetch one, so I couldn't do a real compile. Instead I transcribed the
mister state machine into a standalone C++ harness
(`mister_sim.cpp` / `mister_test.cpp` in the outputs folder) containing *both* the
old and new logic, and ran every command sequence up to length 4 across all eight
command types, crossed with both germination states and five times of day —
46,800 scenarios. The property checked: once commands stop arriving, the relay
must reach OFF within a bounded time.

```
scenarios checked : 46800
safety bound      : 310500 ms
NEW violations    : 0
OLD violations    : 15500   <- test is proven to catch the original bug
```

The old-logic count matters: a test that passed on both versions would prove
nothing. Worst-case relay-on under the new logic is 310.5 s, which is
`MISTER_SPRAY_DURATION + MISTER_MAX_CONTINUOUS_MS` — a timed one-shot expiring and
chaining into a continuous spray. That's the intended ceiling, not a leak.

Caveat: this validates the *logic*, not that the firmware compiles for ESP32.
Structural checks (brace/paren balance with raw-string handling, declaration
before use, every `firebase*` call resolving to a definition) all pass, but you
should still run `pio run` before flashing.

### M1 — Make the project consistent — DONE (except the Netlify dashboard step)

- [x] `.firebaserc`: `microgreen-tray` -> `microgreen-system`.
- [ ] **Your action, in the browser:** Netlify dashboard -> Site configuration ->
      Environment variables: add
      `FIREBASE_DATABASE_URL = https://microgreen-system-default-rtdb.firebaseio.com`.
      Redeploy after adding — env vars are read at build/deploy time. I can't do
      this one for you; it's the only M1 item left.
- [x] Confirmed `data/firebase-config.js` is committed. That's fine: the Firebase
      web API key is a public project identifier, not a secret. Access control comes
      from database rules, not from hiding this file.
- [x] Moved `STA_SSID` / `STA_PASSWORD` into a gitignored `include/secrets.h`, with
      `include/secrets.example.h` committed as a template. `FIREBASE_DB_BASE` moved
      too, so all environment-specific values sit in one file.
      **Still worth doing: rotate that WiFi password.** Removing the credentials
      from the working tree does not remove them from git history — anyone with the
      repo can still `git log -p` them out.
- [x] Deleted dead Postgres scaffolding: `db/`, `netlify/database/`.
- [x] Renamed the `supabase*` helpers in `main.cpp` to `firebase*` — they already
      talked to Firebase and the names actively misled. Also fixed user-facing
      "Supabase" strings in `data/index.html`.
- [x] Deleted `package-lock.json`. It was stale in a way that would have broken
      your deploy: it declared `@netlify/database` and `drizzle-orm` as root
      dependencies while `package.json` lists only `firebase`, and `firebase`
      appeared nowhere in it. Netlify runs `npm ci` when a lockfile is present, and
      `npm ci` hard-fails on exactly that mismatch. With no lockfile Netlify falls
      back to `npm install`, which succeeds and regenerates a correct one.
- [x] Normalized line endings on the files I touched. The working tree had drifted
      to CRLF while the repo stores LF, so eleven files I never edited were showing
      as fully modified. I restored those and converted my own edits to LF, so
      `git status` now shows only real changes. If this keeps recurring, add a
      `.gitattributes` with `* text=auto`.

### M2 — Fix time and the data shape (firmware)

- [ ] In `setup()`, after WiFi connects:
      `configTime(19800, 0, "pool.ntp.org", "time.nist.gov");` then block briefly
      until `time(nullptr) > 1700000000`. 19800 = UTC+5:30 for Colombo.
      Note `configTime` appears **zero** times in `main.cpp` today, yet line 1765
      already calls `time(nullptr)` / `localtime()` to drive the 6am and 6pm mister
      schedule. Without NTP that clock starts at the epoch on every boot, so the
      "6am" spray actually fires roughly six hours after power-on. Fixing NTP fixes
      the schedule as a side effect.
- [ ] Add `uint64_t nowMs()` returning epoch milliseconds. Replace every
      `millis()/1000` used as a *timestamp* with it. Leave `millis()` alone where
      it measures elapsed intervals — that's correct usage.
- [ ] `updateFirebaseHeartbeat()` and the new `/live` writer: use `PUT`, not `POST`.
- [ ] Publish full state to `/live` every 2-3 s (this is what the dashboard reads).
      Keep `/history` appends at 5 min.
- [ ] After executing a command, `DELETE /commands/<id>.json` instead of patching
      `status: "executed"`. Today `/commands` grows forever and the device GETs the
      **entire** node every poll into a `StaticJsonDocument<1024>` — after a few
      hundred commands that silently overflows and control stops working. This is a
      latent bug that will bite in a few weeks of use.

### M3 — Make the dashboard realtime (this is the payoff)

- [ ] In `data/index.html`, replace the 3-second `setInterval(updateStatus, 3000)`
      polling with a live subscription:

```js
firebaseDb.ref('live').on('value', snap => {
  const d = snap.val();
  if (!d) return;
  renderTelemetry(d);                      // reuse the existing render code
  const age = Date.now() - d.ts;
  setConnPill(age < 15000 ? 'online' : 'stale');
});
```

`.on('value')` pushes over a websocket, so updates land in well under a second
and cost zero Netlify invocations.

- [ ] Write commands straight from the browser — the Netlify function round-trip
      adds latency for no benefit while rules are open:

```js
firebaseDb.ref('commands').push({
  action: 'pump_on', status: 'pending', ts: Date.now()
});
```

- [ ] Derive online/offline from `Date.now() - d.ts` against the 2-3 s publish
      interval. Drop the `/.netlify/functions/motor-control` GET path and the
      "Netlify Proxy" / `ESP32_CONTROL_URL` UI copy — that was for the old
      reach-the-device-directly design, which cannot work across networks.
- [ ] Keep the ⚙️ Target direct-IP box. It's genuinely useful on the home LAN when
      the internet is down.
- [ ] Point the charts at `/history` via the Firebase SDK instead of
      `/.netlify/functions/history`.

At this point: `git push`, Netlify auto-deploys `data/`, and the dashboard works
from mobile data anywhere. **Milestones 4 and 5 are improvements, not blockers.**

### M4 — Responsiveness and the blocking problem (firmware)

Right now every Firebase call runs inline in `loop()` and opens a fresh
`WiFiClientSecure`. Each TLS handshake blocks for roughly 1-2 s, and during that
window `loop()` is frozen — which means the flow-rate safety cutoff
(`flow_ml_min > FLOW_THRESHOLD` -> relay off), the float switch read, and the
10-second mister timer all stall. Tightening the command poll to get snappier
buttons makes this *worse*, so the two have to be solved together.

- [ ] Move all Firebase I/O to a dedicated FreeRTOS task pinned to core 0:
      `xTaskCreatePinnedToCore(networkTask, "net", 8192, NULL, 1, NULL, 0);`
      `loop()` runs on core 1 and stops blocking on network entirely. Guard shared
      state (`pumpMode`, `flow_ml_min`, ...) with a mutex or make them `volatile`
      and only written from one side.
- [ ] Then either keep a 2 s REST poll (simple), or subscribe to `/commands` with
      RTDB REST streaming — `Accept: text/event-stream` gives instant push. If you
      stream, you must follow the **HTTP 307 redirect** Firebase issues on the
      initial request, or the stream silently fails.
- [ ] Add a pump watchdog independent of the network: hard-stop the relay after
      N minutes of continuous run. Today `pumpMode == "ManualOn"` holds the relay
      closed indefinitely and the *only* thing that stops it is the flow-rate
      check. If the flow sensor fails open and reads 0, nothing stops the pump.
      If WiFi drops while the pump is manually on, nothing turns it off either.

### M5 — Guard rails

- [ ] Prune `/history` (server-side, or a scheduled Netlify function) to ~10 days.
      Spark tier is 1 GB stored and 10 GB/month downloaded; a 5-minute append is
      about 105k records/year, which is comfortable, but unbounded growth plus the
      old unpruned `/sensorReadings` is not.
- [ ] Delete the existing `/sensorReadings` and `/deviceStatus` nodes once `/live`
      and `/history` are live.

## About the open database

You chose no login, and I've planned for that — but you should know exactly what
it means, because this isn't a website where the worst case is a defaced page.

`database.rules.json` is `.read: true, .write: true`. Your database URL ships
inside `data/firebase-config.js`, which is public by design. So anyone who views
source on your Netlify site can write `/commands` and start your pump, or delete
the entire database. Firebase will also email you automated insecure-rules
warnings. This isn't hypothetical: open RTDB instances get found by automated
scanners that crawl for exactly this.

Two things worth doing even if you keep it open:

**The watchdog in M4 is the real protection.** A device-side maximum run time
means the worst case is a short pump cycle rather than a flooded tray and a
burnt-out pump. That guards against firmware bugs and dropped WiFi too, not just
strangers. Do this one regardless.

**Validation rules cost nothing and stop the crude attacks.** Even fully open,
you can constrain the shape of what gets written so `/commands` only accepts
known actions and nobody can dump junk into your quota:

```json
{
  "rules": {
    "live":     { ".read": true,  ".write": false },
    "history":  { ".read": true,  ".write": false },
    "commands": {
      ".read": true, ".write": true,
      "$cmd": {
        "action": {
          ".validate": "newData.isString() && newData.val().length < 24"
        }
      }
    }
  }
}
```

Note `.write: false` on `/live` and `/history`: the ESP32 writes those over REST
with a database secret or via an authenticated path, while browsers can only
read them. That alone removes the "someone wipes my sensor history" case.

If you later want to close the write path properly, the cheapest upgrade is a
shared passphrase checked in a Netlify function that holds the only write
credential — roughly an hour of work. Anonymous Firebase Auth is *not* a
meaningful boundary here, because anyone can mint an anonymous account in your
project; `auth != null` would stop scrapers and nothing more.

## Corrections to note

Two things I assumed that turned out to be wrong, both verified against current
docs:

- **`mobizt/Firebase-ESP-Client` is deprecated** and end-of-life. If you add a
  Firebase library rather than hand-rolling REST, use the successor:
  `mobizt/FirebaseClient@^2.2.12`. It's async-first and network-independent since
  2.1.0, so pre-2.1 examples you find online won't compile as written.
- **Netlify `.cjs` functions are on a deprecation path.** CommonJS only runs under
  Lambda compatibility mode. Your two functions are `.cjs` with `exports.handler`.
  They still work, but if you keep any function past M3, port it to ESM (`.mjs`,
  default export, `Request`/`Response`). Default Node for new sites is now 24.

## Open question

`data/` is both the Netlify publish directory and the SPIFFS image flashed to the
ESP32, so the dashboard and `firebase-config.js` get written to the board's flash
too. Separately, `main.cpp` serves its *own* hardcoded copy of an older dashboard
from `handleRoot()` (`main.cpp:711`) rather than serving `data/index.html` from
SPIFFS — so the local page and the cloud page have drifted apart and now look
different. Worth consolidating: either serve `data/index.html` from SPIFFS and
delete the 450-line string literal, or split into `web/` (Netlify) and `data/`
(SPIFFS).

