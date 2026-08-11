# Flow sensor & pump interlock verification

Host-side test for the two pieces of logic changed in `src/main.cpp`:

1. **Flow rate calculation** — must derive the rate from the *measured* window length,
   not a hard-coded 1000 ms. `loop()` can overrun by seconds (a Firebase HTTPS call
   has an 8 s timeout), and the old code divided by 1 s regardless, under-reporting
   flow by exactly the overrun factor.
2. **Pump decision table** — the drain-line flow interlock must stop the pump in Auto
   mode, while Manual On is still able to override it.
3. **Fan trigger** — the fans run when *either* temperature > 28 °C or humidity > 60 %
   (previously both were required simultaneously), with a hysteresis deadband so a
   reading sitting on the threshold cannot chatter the relay.

## Run

```bash
cd tools/flow_pump_verification
g++ -std=c++17 -O2 -o flow_pump_test flow_pump_test.cpp
./flow_pump_test
```

On Windows with MinGW or MSVC:

```powershell
g++ -std=c++17 -O2 -o flow_pump_test.exe flow_pump_test.cpp
.\flow_pump_test.exe
```

Exit code 0 means every check passed.

## Keeping it in sync

The constants at the top of `flow_pump_test.cpp` mirror `src/main.cpp`. If you change
`PULSES_PER_LITER`, `FLOW_DETECT_ML_MIN`, `FLOW_FAULT_CLEAR_MS`, `SOIL_THRESHOLD`,
`TEMP_THRESHOLD`, `HUMIDITY_THRESHOLD`, `TEMP_HYSTERESIS` or `HUMIDITY_HYSTERESIS`
in the firmware, update them here too.

## On-hardware checks

The test cannot see the sensor, so confirm these over the serial monitor at 115200:

- The status line now prints `Flow: <rate> ml/min (<pulses> pulses / <window> ms,
  total <n>) | Interlock: clear|FAULT`.
- Blow through the flow sensor, or pour water down the drain line. `pulses` should
  climb. If it stays at 0 the problem is wiring, not firmware — check that the
  sensor's signal wire is on GPIO27 and that the sensor is powered from 5 V while
  its signal line is safe for the ESP32's 3.3 V input.
- With flow present, `Interlock: FAULT` should appear and the pump relay should go
  off in Auto mode. Pressing **ON** in the dashboard should still start the pump.
