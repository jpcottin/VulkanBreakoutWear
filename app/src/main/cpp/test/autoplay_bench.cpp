// Auto Play bench: plays seeded, headless rounds with the autopilot and
// reports how often it clears each level, how long it takes and how many
// balls it loses. Every Auto Play change is measured here before it ships
// (the numbers in the commit messages come from this program).
//
// Builds on the host, no Android SDK needed: scripts/autoplay-bench.sh.
//
//   autoplay_bench [--runs N] [--width W --height H] [--levels] [--campaigns]
//                  [--min-campaign-win PCT] [--trace]
//
// --levels    plays each level 1..10 on its own, N seeds each, starting with
//             five balls (the default when nothing is selected).
// --campaigns plays N full campaigns from level 1.
// --min-campaign-win PCT exits 1 when fewer than PCT% of campaigns are won:
//             the CI regression guard.
// --trace     prints the scene (balls, paddle, autopilot intent) at every
//             lost ball.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "game.h"

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kLevelTimeout = 480.0f;     // seconds before a level counts as stuck
bool gTrace = false;

struct Stats {
    int runs = 0, cleared = 0, timeouts = 0;
    int ballsLost = 0;
    double seconds = 0.0;     // summed over cleared runs
    long score = 0;
    long bricks = 0, paddleHits = 0;
};

void dumpScene(const Game& g, float t) {
    printf("  lost ball L%d t=%.1f paddle x=%.3f (hw %.3f) aiTarget=%.3f move=%d"
           " landing=%d x=%.3f t=%.2f u=%.2f balls=%d bricks left=%d\n",
           g.level(), t, g.paddleX(), g.paddleHalfWForTest(), g.aiTargetXForTest(),
           g.aiHasTargetForTest(), g.aiHasLandingForTest(), g.aiLandingXForTest(),
           g.aiLandingTForTest(), g.aiPlanUForTest(), g.ballCount(), g.breakableLeftForTest());
    for (int i = 0; i < g.ballCount(); i++)
        printf("    ball x=%.3f y=%.3f dir=(%.2f, %.2f)\n",
               g.ballXForTest(i), g.ballYForTest(i), g.ballDxForTest(i), g.ballDyForTest(i));
}

// Plays from the current PLAYING state until the level ends or the game does.
// Returns true when the level was cleared (LEVEL_CLEAR or WIN).
bool playLevel(Game& g, Stats& s, float& seconds) {
    seconds = 0.0f;
    int lost0 = g.ballsLostForTest(), bricks0 = g.bricksBrokenForTest(), hits0 = g.paddleHitsForTest();
    int lost = lost0;
    Game before = g;               // the frame before a lost ball, for --trace
    while (g.isPlayingForTest()) {
        if (gTrace) before = g;
        g.update(kDt);
        seconds += kDt;
        if (g.ballsLostForTest() > lost) {
            lost = g.ballsLostForTest();
            if (gTrace) dumpScene(before, seconds);
        }
        if (seconds > kLevelTimeout) {
            s.timeouts++;
            if (gTrace) { printf("  timeout:\n"); dumpScene(g, seconds); }
            break;
        }
    }
    s.ballsLost  += g.ballsLostForTest() - lost0;
    s.bricks     += g.bricksBrokenForTest() - bricks0;
    s.paddleHits += g.paddleHitsForTest() - hits0;
    return g.isLevelClearForTest() || g.isWinForTest();
}

void printStats(const char* label, const Stats& s) {
    double avg = s.cleared ? s.seconds / s.cleared : 0.0;
    printf("%-10s cleared %5.1f%%  balls lost/run %.2f  timeouts %2d  avg %6.1f s"
           "  bricks/min %5.1f  paddle hits/run %5.1f  avg score %7ld\n",
           label, 100.0 * s.cleared / s.runs, (double)s.ballsLost / s.runs, s.timeouts, avg,
           s.seconds > 0.0 ? 60.0 * s.bricks / (s.seconds + 1e-9) : 0.0,
           (double)s.paddleHits / s.runs, s.runs ? s.score / s.runs : 0L);
}

}  // namespace

int main(int argc, char** argv) {
    int runs = 100, w = 480, h = 480;
    bool levels = false, campaigns = false;
    double minCampaignWin = -1.0;
    for (int i = 1; i < argc; i++) {
        auto next = [&](int& dst) { if (i + 1 < argc) dst = atoi(argv[++i]); };
        if      (!strcmp(argv[i], "--runs"))      next(runs);
        else if (!strcmp(argv[i], "--width"))     next(w);
        else if (!strcmp(argv[i], "--height"))    next(h);
        else if (!strcmp(argv[i], "--levels"))    levels = true;
        else if (!strcmp(argv[i], "--campaigns")) campaigns = true;
        else if (!strcmp(argv[i], "--trace"))     gTrace = true;
        else if (!strcmp(argv[i], "--min-campaign-win") && i + 1 < argc)
            minCampaignWin = atof(argv[++i]);
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (minCampaignWin >= 0.0) campaigns = true;
    if (!levels && !campaigns) levels = true;
    if (runs <= 0 || w <= 0 || h <= 0) { fprintf(stderr, "bad size or run count\n"); return 2; }
    printf("Auto Play bench: %dx%d, %d seeded runs\n", w, h, runs);

    if (levels) {
        Stats total;
        for (int L = 1; L <= 10; L++) {
            Stats s;
            for (int seed = 1; seed <= runs; seed++) {
                Game g;
                g.setViewport(w, h);
                g.seedRng((uint32_t)seed * 2654435761u);
                g.triggerNewGameForTest();
                if (L > 1) g.startLevelForTest(L);
                g.setAutoPlayForTest(true);
                float sec;
                s.runs++;
                if (playLevel(g, s, sec)) { s.cleared++; s.seconds += sec; }
                s.score += g.score();
            }
            char label[16];
            snprintf(label, sizeof(label), "level %2d", L);
            printStats(label, s);
            total.runs += s.runs; total.cleared += s.cleared; total.timeouts += s.timeouts;
            total.ballsLost += s.ballsLost; total.seconds += s.seconds; total.score += s.score;
            total.bricks += s.bricks; total.paddleHits += s.paddleHits;
        }
        printStats("all", total);
    }

    int won = 0;
    if (campaigns) {
        Stats s;
        double wonSeconds = 0.0;
        int reached[11] = {};
        for (int seed = 1; seed <= runs; seed++) {
            Game g;
            g.setViewport(w, h);
            g.seedRng((uint32_t)seed * 2654435761u);
            g.triggerNewGameForTest();
            g.setAutoPlayForTest(true);
            float total = 0.0f;
            s.runs++;
            while (true) {
                float sec;
                reached[g.level()]++;
                bool ok = playLevel(g, s, sec);
                total += sec;
                if (!ok || g.isWinForTest()) break;
                // LEVEL_CLEAR: let the interlude run into the next level.
                while (g.isLevelClearForTest()) g.update(kDt);
            }
            if (g.isWinForTest()) { won++; wonSeconds += total; }
            s.score += g.score();
        }
        printf("campaigns  won %5.1f%%  balls lost/run %.2f  timeouts %2d  avg won %6.1f s"
               "  avg score %7ld\n",
               100.0 * won / s.runs, (double)s.ballsLost / s.runs, s.timeouts,
               won ? wonSeconds / won : 0.0, s.score / s.runs);
        printf("reached    ");
        for (int L = 1; L <= 10; L++) printf("L%d %3d  ", L, reached[L]);
        printf("\n");
        if (minCampaignWin >= 0.0 && 100.0 * won / s.runs < minCampaignWin) {
            printf("FAIL: campaign win rate below %.1f%%\n", minCampaignWin);
            return 1;
        }
    }
    return 0;
}
