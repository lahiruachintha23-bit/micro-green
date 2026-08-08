// Faithful transcription of the mister state machine from src/main.cpp.
// Purpose: prove that once commands stop arriving, the relay always reaches OFF
// within a bounded time. Built with OLD and NEW logic so we can confirm the test
// actually catches the original latch bug instead of passing vacuously.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static const unsigned long MISTER_SPRAY_DURATION    = 10000;
static const unsigned long MISTER_MAX_CONTINUOUS_MS = 300000;
static const int  GERMINATION_PERIOD_DAYS = 3;

static unsigned long g_ms = 0;
static unsigned long millis() { return g_ms; }

enum Cmd { NONE, CLOUD_SPRAY, CLOUD_MISTER_OFF, CLOUD_MISTER_AUTO,
           LOCAL_SPRAY, MODE_AUTO, MODE_MANUAL_ON, MODE_MANUAL_OFF };

struct Sim {
    bool NEW;                       // true = post-fix logic
    std::string misterMode = "Auto";
    bool  inProgress = false;
    bool  timed      = false;       // NEW only
    unsigned long startTime = 0;
    bool  relayOn = false;          // true == relay driven LOW == misting
    bool  spray6amDone = false, spray6pmDone = false;

    void startOneShot() {           // startMisterOneShot()
        relayOn = true; inProgress = true; timed = true; startTime = millis();
    }
    void stopNow() {                // stopMisterNow()
        relayOn = false; inProgress = false; timed = false;
    }

    void command(Cmd c) {
        if (NEW) {
            switch (c) {
                case CLOUD_SPRAY: case LOCAL_SPRAY: startOneShot(); break;
                case CLOUD_MISTER_OFF: misterMode = "ManualOff"; stopNow(); break;
                case CLOUD_MISTER_AUTO: case MODE_AUTO: misterMode = "Auto"; break;
                case MODE_MANUAL_ON:  misterMode = "ManualOn";  break;
                case MODE_MANUAL_OFF: misterMode = "ManualOff"; stopNow(); break;
                default: break;
            }
        } else {
            // OLD: cloud spray latched the mode to ManualOn; local spray set no mode.
            switch (c) {
                case CLOUD_SPRAY:
                    misterMode = "ManualOn";
                    relayOn = true; inProgress = true; startTime = millis(); break;
                case LOCAL_SPRAY:
                    relayOn = true; inProgress = true; startTime = millis(); break;
                case CLOUD_MISTER_OFF:
                    misterMode = "ManualOff";
                    relayOn = false; inProgress = false; break;
                case CLOUD_MISTER_AUTO: case MODE_AUTO: misterMode = "Auto"; break;
                case MODE_MANUAL_ON:  misterMode = "ManualOn";  break;
                case MODE_MANUAL_OFF: misterMode = "ManualOff";
                                      relayOn = false; inProgress = false; break;
                default: break;
            }
        }
    }

    void tick(bool shouldMisterWork, int hour) {
        if (NEW) {
            if (inProgress && timed && (millis() - startTime >= MISTER_SPRAY_DURATION)) {
                stopNow();
            } else if (inProgress && !timed &&
                       (millis() - startTime >= MISTER_MAX_CONTINUOUS_MS)) {
                misterMode = "ManualOff"; stopNow();
            }
            if (misterMode == "ManualOn" && !inProgress) {
                relayOn = true; inProgress = true; timed = false; startTime = millis();
            } else if (misterMode == "ManualOff" && inProgress && !timed) {
                stopNow();
            } else if (misterMode == "Auto" && shouldMisterWork) {
                schedule(hour);
            } else if (misterMode == "Auto") {
                if (inProgress && !timed) stopNow();
            }
        } else {
            if (misterMode == "ManualOn" && !inProgress) {
                relayOn = true; inProgress = true; startTime = millis();
            } else if (misterMode == "ManualOff" && inProgress) {
                relayOn = false; inProgress = false;
            } else if (misterMode == "Auto" && shouldMisterWork) {
                schedule(hour);
                // OLD: the auto-off lived HERE, inside the Auto branch.
                if (inProgress && (millis() - startTime >= MISTER_SPRAY_DURATION)) {
                    relayOn = false; inProgress = false;
                }
            } else if (misterMode == "Auto") {
                if (inProgress) { relayOn = false; inProgress = false; }
            }
        }
    }

    void schedule(int hour) {
        if (hour == 6 && !spray6amDone) {
            if (!inProgress) { if (NEW) startOneShot();
                               else { relayOn = true; inProgress = true; startTime = millis(); } }
            spray6amDone = true; spray6pmDone = false;
        } else if (hour == 18 && !spray6pmDone) {
            if (!inProgress) { if (NEW) startOneShot();
                               else { relayOn = true; inProgress = true; startTime = millis(); } }
            spray6pmDone = true; spray6amDone = false;
        } else if (hour != 6 && hour != 18) {
            spray6amDone = false; spray6pmDone = false;
        }
    }
};

static const unsigned long TICK = 50;   // loop() period
// Once commands stop, the relay must be off within this long.
// Worst legitimate case is a timed one-shot that expires and is then followed by
// a continuous spray (mode was already ManualOn), so the two durations chain.
// The continuous ceiling forces mode to ManualOff, which terminates the chain.
static const unsigned long BOUND =
    MISTER_SPRAY_DURATION + MISTER_MAX_CONTINUOUS_MS + 10 * TICK;

// Returns 0 on pass, or the ms the relay stayed on past BOUND.
// Also records the longest uninterrupted relay-ON streak seen, so we can report
// the real worst case rather than only a pass/fail against BOUND.
static unsigned long g_maxOnStreak = 0;

static unsigned long run(bool useNew, const std::vector<Cmd>& cmds,
                         bool shouldWork, int hour, std::string* why) {
    g_ms = 100000;                    // non-zero start: catches millis() rollover-ish assumptions
    Sim s; s.NEW = useNew;
    unsigned long onSince = 0; bool wasOn = false;
    auto sample = [&]() {
        if (s.relayOn && !wasOn) { onSince = g_ms; wasOn = true; }
        else if (!s.relayOn && wasOn) {
            if (g_ms - onSince > g_maxOnStreak) g_maxOnStreak = g_ms - onSince;
            wasOn = false;
        }
    };
    for (Cmd c : cmds) {              // issue commands, letting loop() run between them
        s.command(c); sample();
        for (int i = 0; i < 4; ++i) { g_ms += TICK; s.tick(shouldWork, hour); sample(); }
    }
    unsigned long quietStart = g_ms;  // commands stop here; system must go safe
    while (g_ms - quietStart <= BOUND) { g_ms += TICK; s.tick(shouldWork, hour); sample(); }
    if (wasOn && g_ms - onSince > g_maxOnStreak) g_maxOnStreak = g_ms - onSince;
    if (s.relayOn) {
        *why = "relay STILL ON " + std::to_string(BOUND) + "ms after last command"
               " (mode=" + s.misterMode + ")";
        return BOUND;
    }
    return 0;
}
