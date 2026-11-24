// maxflow_test -- mesh/MaxFlow.h against brute force: on thousands of small
// random graphs the flow must equal the minimum over every s-t labelling, and
// the labelling the solver returns must cut exactly that much. Integer
// capacities held in float, so both sides are exact.

#include "mesh/MaxFlow.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) std::printf("FAIL %s\n", what.c_str());
    if (!ok) g_failures++;
}

struct Edge { int i, j; double c, r; };

double cut_cost(int n, const std::vector<double>& src, const std::vector<double>& snk,
                const std::vector<Edge>& edges, uint32_t in_source_mask) {
    double cost = 0;
    for (int i = 0; i < n; i++) {
        const bool s = (in_source_mask >> i) & 1;
        cost += s ? snk[(size_t)i] : src[(size_t)i];
    }
    for (const Edge& e : edges) {
        const bool si = (in_source_mask >> e.i) & 1, sj = (in_source_mask >> e.j) & 1;
        if (si && !sj) cost += e.c;
        if (sj && !si) cost += e.r;
    }
    return cost;
}

template <typename Cap>
void run(int trials, uint32_t seed, const char* tag) {
    std::mt19937 rng(seed);
    int ok_flow = 0;
    for (int t = 0; t < trials; t++) {
        const int n = 1 + (int)(rng() % 10);
        const int m = (int)(rng() % (3 * n + 1));
        std::vector<double> src((size_t)n, 0), snk((size_t)n, 0);
        std::vector<Edge> edges;
        meshing::MaxFlow<Cap> g(n);
        for (int i = 0; i < n; i++) {
            // Several add_tweights calls per node, as the mesher makes them.
            for (int rep = 0; rep < 2; rep++) {
                const double a = (rng() % 3 == 0) ? 0 : (double)(rng() % 9);
                const double b = (rng() % 3 == 0) ? 0 : (double)(rng() % 9);
                src[(size_t)i] += a;
                snk[(size_t)i] += b;
                g.add_tweights(i, (Cap)a, (Cap)b);
            }
        }
        for (int k = 0; k < m; k++) {
            const int i = (int)(rng() % n), j = (int)(rng() % n);
            if (i == j) continue;
            const double c = (double)(rng() % 7), r = (rng() % 2) ? (double)(rng() % 7) : 0.0;
            edges.push_back({i, j, c, r});
            g.add_edge(i, j, (Cap)c, (Cap)r);
        }
        double best = 1e300;
        for (uint32_t mask = 0; mask < (1u << n); mask++) best = std::min(best, cut_cost(n, src, snk, edges, mask));
        const double flow = (double)g.solve();
        uint32_t got = 0;
        for (int i = 0; i < n; i++) got |= (uint32_t)g.in_source(i) << i;
        const double got_cost = cut_cost(n, src, snk, edges, got);
        const bool good = std::fabs(flow - best) < 1e-6 && std::fabs(got_cost - best) < 1e-6;
        check(good, std::string(tag) + " trial " + std::to_string(t) + ": flow " + std::to_string(flow) +
                        ", min cut " + std::to_string(best) + ", returned cut " + std::to_string(got_cost));
        ok_flow += good;
    }
    std::printf("%s %s: %d of %d graphs match brute force\n", ok_flow == trials ? "ok  " : "FAIL", tag, ok_flow, trials);
}

// A long chain and a grid: deep trees, many orphans, the adoption path.
void structured() {
    const int n = 4000;
    meshing::MaxFlow<float> chain(n);
    for (int i = 0; i + 1 < n; i++) chain.add_edge(i, i + 1, 5.0f + (float)(i % 3), 5.0f);
    chain.add_tweights(0, 100, 0);
    chain.add_tweights(n - 1, 0, 100);
    const float f = chain.solve();
    check(f == 5.0f, "chain: flow is the weakest link");
    std::printf("%s chain of %d: flow %g\n", f == 5.0f ? "ok  " : "FAIL", n, (double)f);

    const int W = 60, H = 60;
    meshing::MaxFlow<double> grid(W * H);
    std::mt19937 rng(7);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const int i = y * W + x;
            if (x + 1 < W) grid.add_edge(i, i + 1, 1 + rng() % 4, 1 + rng() % 4);
            if (y + 1 < H) {
                const double c = 1 + rng() % 4;
                grid.add_edge(i, i + W, c, c);
            }
            if (y == 0) grid.add_tweights(i, 1000, 0);
            if (y == H - 1) grid.add_tweights(i, 0, 1000);
        }
    const double gf = grid.solve();
    int src_rows_ok = 1;
    for (int x = 0; x < W; x++) src_rows_ok &= grid.in_source(x) && !grid.in_source((H - 1) * W + x);
    check(gf > 0 && gf < 4.0 * W + 1 && src_rows_ok, "grid: a cut between the terminal rows");
    std::printf("%s grid %dx%d: flow %g\n", src_rows_ok ? "ok  " : "FAIL", W, H, gf);
}

}  // namespace

int main() {
    run<float>(3000, 1u, "float");
    run<double>(3000, 2u, "double");
    structured();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
