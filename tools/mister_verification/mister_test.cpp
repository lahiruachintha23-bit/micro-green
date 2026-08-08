// Driver: exhaustive over all command sequences up to length 4, crossed with
// every (shouldMisterWork, hour) environment. Asserts the safety property on the
// NEW logic and asserts the OLD logic VIOLATES it (so we know the test has teeth).
#include "mister_sim.cpp"

static const char* name(Cmd c) {
    switch (c) {
        case NONE: return "-";
        case CLOUD_SPRAY: return "cloud:spray";
        case CLOUD_MISTER_OFF: return "cloud:mister_off";
        case CLOUD_MISTER_AUTO: return "cloud:mister_auto";
        case LOCAL_SPRAY: return "local:spray";
        case MODE_AUTO: return "mode:Auto";
        case MODE_MANUAL_ON: return "mode:ManualOn";
        case MODE_MANUAL_OFF: return "mode:ManualOff";
    }
    return "?";
}

int main() {
    const Cmd ALL[] = { NONE, CLOUD_SPRAY, CLOUD_MISTER_OFF, CLOUD_MISTER_AUTO,
                        LOCAL_SPRAY, MODE_AUTO, MODE_MANUAL_ON, MODE_MANUAL_OFF };
    const int N = 8;
    const int HOURS[] = { 5, 6, 12, 18, 23 };

    long newChecked = 0, newFail = 0, oldFail = 0;
    std::string firstNewFail, firstOldFail;
    unsigned long newMaxOn = 0, oldMaxOn = 0;

    for (int len = 1; len <= 4; ++len) {
        std::vector<int> idx(len, 0);
        while (true) {
            std::vector<Cmd> cmds;
            for (int i = 0; i < len; ++i) cmds.push_back(ALL[idx[i]]);

            for (int w = 0; w < 2; ++w) {
                for (int h : HOURS) {
                    std::string why;
                    // NEW logic must always go safe.
                    g_maxOnStreak = 0;
                    if (run(true, cmds, w == 1, h, &why)) {
                        if (!newFail) {
                            firstNewFail = why + " | seq:";
                            for (Cmd c : cmds) firstNewFail += std::string(" ") + name(c);
                            firstNewFail += " | shouldWork=" + std::to_string(w)
                                          + " hour=" + std::to_string(h);
                        }
                        ++newFail;
                    }
                    if (g_maxOnStreak > newMaxOn) newMaxOn = g_maxOnStreak;
                    ++newChecked;
                    // OLD logic: record violations to prove the test is meaningful.
                    std::string why2;
                    g_maxOnStreak = 0;
                    if (run(false, cmds, w == 1, h, &why2)) {
                        if (!oldFail) {
                            firstOldFail = why2 + " | seq:";
                            for (Cmd c : cmds) firstOldFail += std::string(" ") + name(c);
                            firstOldFail += " | shouldWork=" + std::to_string(w)
                                          + " hour=" + std::to_string(h);
                        }
                        ++oldFail;
                    }
                    if (g_maxOnStreak > oldMaxOn) oldMaxOn = g_maxOnStreak;
                }
            }
            int p = len - 1;
            while (p >= 0 && ++idx[p] == N) { idx[p] = 0; --p; }
            if (p < 0) break;
        }
    }

    printf("scenarios checked : %ld\n", newChecked);
    printf("safety bound      : %lu ms\n", BOUND);
    printf("NEW violations    : %ld\n", newFail);
    printf("NEW worst relay-ON: %lu ms\n", newMaxOn);
    printf("OLD violations    : %ld  (expected > 0)\n", oldFail);
    printf("OLD worst relay-ON: %lu ms (unbounded -- capped by sim horizon)\n", oldMaxOn);
    if (!firstOldFail.empty()) printf("  old example: %s\n", firstOldFail.c_str());
    if (!firstNewFail.empty()) printf("  NEW FAILURE: %s\n", firstNewFail.c_str());

    bool ok = (newFail == 0) && (oldFail > 0);
    printf("\n%s\n", ok ? "PASS: new logic always reaches OFF; test proven to catch the old bug"
                        : "FAIL");
    return ok ? 0 : 1;
}
