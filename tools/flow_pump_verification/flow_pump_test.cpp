// Host-side verification for the flow-sensor maths and the pump decision table in
// src/main.cpp. This does not touch hardware; it re-implements the two pieces of
// pure logic that were changed so their behaviour can be asserted on a PC.
//
//   g++ -std=c++17 -O2 -o flow_pump_test flow_pump_test.cpp && ./flow_pump_test
//
// Keep the constants below in sync with src/main.cpp.

#include <cstdio>
#include <cmath>
#include <string>

// ---- Constants mirrored from src/main.cpp ----
static const double PULSES_PER_LITER = 450.0;
static const double FLOW_DETECT_ML_MIN = 60.0;
static const unsigned long FLOW_FAULT_CLEAR_MS = 8000;
static const int SOIL_THRESHOLD = 2000;

static int failures = 0;

static void check(bool cond, const char *what)
{
    if (cond)
    {
        printf("  PASS  %s\n", what);
    }
    else
    {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

static void checkNear(double actual, double expected, double tol, const char *what)
{
    bool ok = std::fabs(actual - expected) <= tol;
    if (ok)
    {
        printf("  PASS  %s (got %.2f)\n", what, actual);
    }
    else
    {
        printf("  FAIL  %s (got %.2f, expected %.2f +/- %.2f)\n", what, actual, expected, tol);
        failures++;
    }
}

// ---- Logic under test: flow rate from a MEASURED window ----
static double flowMlMin(unsigned long pulses, unsigned long elapsedMs)
{
    double litres = (double)pulses / PULSES_PER_LITER;
    return litres * 1000.0 * (60000.0 / (double)elapsedMs);
}

// ---- Logic under test: latching overflow interlock ----
struct Interlock
{
    bool fault = false;
    unsigned long lastFlowSeen = 0;

    void update(double flow, unsigned long nowMs)
    {
        if (flow >= FLOW_DETECT_ML_MIN)
        {
            lastFlowSeen = nowMs;
            fault = true;
        }
        else if (fault && (nowMs - lastFlowSeen >= FLOW_FAULT_CLEAR_MS))
        {
            fault = false;
        }
    }
};

// ---- Logic under test: pump relay decision ----
// Returns true when the pump should be energised.
static bool pumpShouldRun(const std::string &pumpMode, bool flowFault, int soilValue)
{
    if (pumpMode == "ManualOn")
        return true; // manual outranks the interlock, by design
    if (pumpMode == "ManualOff")
        return false;
    if (flowFault)
        return false; // Auto defers to the overflow interlock
    return soilValue > SOIL_THRESHOLD;
}

int main()
{
    printf("== Flow rate uses the measured window, not an assumed 1000 ms ==\n");
    // 8 pulses in a true 1 s window: 8/450 L in 1/60 min => ~1066.7 ml/min
    checkNear(flowMlMin(8, 1000), 1066.67, 0.5, "8 pulses / 1000 ms");
    // The regression that hid real flow: the loop overran to 4 s but the old code
    // still divided by 1 s. The same 8 pulses spread over 4 s is a QUARTER the rate.
    checkNear(flowMlMin(8, 4000), 266.67, 0.5, "8 pulses / 4000 ms (loop overran)");
    check(flowMlMin(8, 4000) < flowMlMin(8, 1000),
          "a longer window yields a lower rate for the same pulse count");
    // A slow drip must still clear the detection floor rather than rounding to zero.
    checkNear(flowMlMin(1, 1000), 133.33, 0.5, "single pulse / 1000 ms");
    check(flowMlMin(1, 1000) >= FLOW_DETECT_ML_MIN,
          "one pulse per second is detected as flow");
    check(flowMlMin(0, 1000) == 0.0, "no pulses reads exactly zero");

    printf("\n== Overflow interlock latches and self-clears ==\n");
    Interlock lock;
    lock.update(0.0, 1000);
    check(!lock.fault, "no fault while the drain line is dry");
    lock.update(200.0, 2000);
    check(lock.fault, "fault latches as soon as drain flow appears");
    lock.update(0.0, 5000);
    check(lock.fault, "fault stays latched 3 s after flow stops");
    lock.update(0.0, 9999);
    check(lock.fault, "fault still latched just under the clear delay");
    lock.update(0.0, 10001);
    check(!lock.fault, "fault clears once the line has been dry for 8 s");
    // A brief burst mid-drain must restart the clear timer.
    lock.update(500.0, 11000);
    lock.update(0.0, 15000);
    check(lock.fault, "a new burst re-arms the latch and restarts the timer");

    printf("\n== Pump decision table ==\n");
    // Auto mode
    check(pumpShouldRun("Auto", false, 2500), "Auto + no fault + wet soil -> ON");
    check(!pumpShouldRun("Auto", false, 1500), "Auto + no fault + dry soil -> OFF");
    check(!pumpShouldRun("Auto", true, 2500), "Auto + OVERFLOW -> OFF (the safety requirement)");
    check(!pumpShouldRun("Auto", true, 1500), "Auto + OVERFLOW + dry soil -> OFF");
    // Manual outranks the interlock
    check(pumpShouldRun("ManualOn", false, 0), "Manual On + no fault -> ON");
    check(pumpShouldRun("ManualOn", true, 0), "Manual On + OVERFLOW -> ON (manual override)");
    check(!pumpShouldRun("ManualOff", false, 5000), "Manual Off + wet soil -> OFF");
    check(!pumpShouldRun("ManualOff", true, 5000), "Manual Off + OVERFLOW -> OFF");

    printf("\n== Regression: the old ordering made the pump uncontrollable ==\n");
    // Old behaviour: flow interlock was evaluated FIRST and returned unconditionally.
    auto oldPumpShouldRun = [](const std::string &mode, bool flowHigh, int soil) {
        if (flowHigh)
            return false; // interlock won every time
        if (mode == "ManualOn")
            return true;
        if (mode == "ManualOff")
            return false;
        return soil > SOIL_THRESHOLD;
    };
    check(!oldPumpShouldRun("ManualOn", true, 0),
          "old logic ignored Manual On whenever flow was reported");
    check(pumpShouldRun("ManualOn", true, 0),
          "new logic honours Manual On under the same conditions");

    printf("\n%s (%d failure%s)\n",
           failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
