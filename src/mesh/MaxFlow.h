#pragma once

// s-t minimum cut by the Boykov-Kolmogorov augmenting-path algorithm
// (Boykov & Kolmogorov, "An Experimental Comparison of Min-Cut/Max-Flow
// Algorithms for Energy Minimization in Vision", PAMI 2004), written from the
// paper: two search trees grown from the terminals, augmentation, adoption.
// Header-only. Neither the maxflow-v3 library nor OpenMVS was used, by licence.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <vector>

namespace meshing {

template <typename Cap>
class MaxFlow {
public:
    explicit MaxFlow(int nodes, int64_t arcs_hint = 0) : nodes_((size_t)nodes) {
        if (arcs_hint > 0) arcs_.reserve((size_t)arcs_hint);
    }

    int num_nodes() const { return (int)nodes_.size(); }

    // Capacities from the source and to the sink, accumulated. Only their
    // difference reaches the graph; the common part is flow already.
    void add_tweights(int i, Cap source, Cap sink) {
        Node& n = nodes_[(size_t)i];
        if (n.tr > 0) source += n.tr; else sink -= n.tr;
        flow_ += std::min(source, sink);
        n.tr = source - sink;
    }

    // i -> j with `cap`, j -> i with `rev_cap`.
    void add_edge(int i, int j, Cap cap, Cap rev_cap) {
        if (i == j) return;
        if (arcs_.size() + 2 > (size_t)std::numeric_limits<int32_t>::max())
            throw std::runtime_error("max-flow graph has too many arcs");
        const int32_t a = (int32_t)arcs_.size();
        arcs_.push_back({j, nodes_[(size_t)i].first, cap});
        nodes_[(size_t)i].first = a;
        arcs_.push_back({i, nodes_[(size_t)j].first, rev_cap});
        nodes_[(size_t)j].first = a + 1;
    }

    Cap solve();

    // After solve(): reachable from the source in the residual graph. Every
    // other node, including one no terminal reaches, is on the sink side.
    bool in_source(int i) const {
        const Node& n = nodes_[(size_t)i];
        return n.parent != kNone && !n.sink;
    }

    Cap flow() const { return flow_; }

private:
    static constexpr int32_t kNone = -1, kTerminal = -2, kOrphan = -3;

    struct Arc { int32_t head, next; Cap cap; };
    struct Node {
        int32_t first = -1, parent = kNone;
        Cap tr = 0;            // > 0: residual from the source; < 0: to the sink
        int32_t ts = 0, dist = 0;
        bool sink = false, active = false;
    };

    std::vector<Node> nodes_;
    std::vector<Arc> arcs_;
    std::deque<int32_t> queue_;
    std::vector<int32_t> orphans_;
    Cap flow_ = 0;
    int32_t time_ = 0;

    static int32_t sister(int32_t a) { return a ^ 1; }

    void activate(int32_t i) {
        Node& n = nodes_[(size_t)i];
        if (n.active) return;
        n.active = true;
        queue_.push_back(i);
    }

    void make_orphan(int32_t i) {
        nodes_[(size_t)i].parent = kOrphan;
        orphans_.push_back(i);
    }

    // The residual capacity a tree edge needs: toward the node in the source
    // tree, away from it in the sink tree.
    Cap tree_cap(const Node& from_tree, int32_t a) const {
        return from_tree.sink ? arcs_[(size_t)a].cap : arcs_[(size_t)sister(a)].cap;
    }

    void augment(int32_t middle);
    void adopt(int32_t i);
};

template <typename Cap>
Cap MaxFlow<Cap>::solve() {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        Node& n = nodes_[i];
        n.parent = kNone;
        n.active = false;
        if (n.tr != 0) {
            n.sink = n.tr < 0;
            n.parent = kTerminal;
            n.ts = 0;
            n.dist = 1;
            activate((int32_t)i);
        }
    }
    while (!queue_.empty()) {
        const int32_t p = queue_.front();
        Node& np = nodes_[(size_t)p];
        if (np.parent == kNone) {
            queue_.pop_front();
            np.active = false;
            continue;
        }
        // Growth: claim free neighbours until the trees touch.
        int32_t middle = -1;
        for (int32_t a = np.first; a >= 0; a = arcs_[(size_t)a].next) {
            const Cap out = np.sink ? arcs_[(size_t)sister(a)].cap : arcs_[(size_t)a].cap;
            if (!(out > 0)) continue;
            const int32_t q = arcs_[(size_t)a].head;
            Node& nq = nodes_[(size_t)q];
            if (nq.parent == kNone) {
                nq.sink = np.sink;
                nq.parent = sister(a);
                nq.ts = np.ts;
                nq.dist = np.dist + 1;
                activate(q);
            } else if (nq.sink != np.sink) {
                middle = np.sink ? sister(a) : a;
                break;
            } else if (nq.ts <= np.ts && nq.dist > np.dist) {
                nq.parent = sister(a);
                nq.ts = np.ts;
                nq.dist = np.dist + 1;
            }
        }
        if (middle < 0) {
            queue_.pop_front();
            np.active = false;
            continue;
        }
        ++time_;
        augment(middle);
        while (!orphans_.empty()) {
            const int32_t o = orphans_.back();
            orphans_.pop_back();
            adopt(o);
        }
    }
    return flow_;
}

template <typename Cap>
void MaxFlow<Cap>::augment(int32_t middle) {
    // `middle` runs from the source tree into the sink tree.
    Cap bottleneck = arcs_[(size_t)middle].cap;
    for (int32_t i = arcs_[(size_t)sister(middle)].head;;) {
        const int32_t a = nodes_[(size_t)i].parent;
        if (a == kTerminal) { bottleneck = std::min(bottleneck, nodes_[(size_t)i].tr); break; }
        bottleneck = std::min(bottleneck, arcs_[(size_t)sister(a)].cap);
        i = arcs_[(size_t)a].head;
    }
    for (int32_t i = arcs_[(size_t)middle].head;;) {
        const int32_t a = nodes_[(size_t)i].parent;
        if (a == kTerminal) { bottleneck = std::min(bottleneck, -nodes_[(size_t)i].tr); break; }
        bottleneck = std::min(bottleneck, arcs_[(size_t)a].cap);
        i = arcs_[(size_t)a].head;
    }
    // x - min(x, ...) is exactly 0 for the saturated edge, so == 0 is safe.
    arcs_[(size_t)sister(middle)].cap += bottleneck;
    arcs_[(size_t)middle].cap -= bottleneck;
    for (int32_t i = arcs_[(size_t)sister(middle)].head;;) {
        Node& n = nodes_[(size_t)i];
        const int32_t a = n.parent;
        if (a == kTerminal) {
            n.tr -= bottleneck;
            if (n.tr == 0) make_orphan(i);
            break;
        }
        arcs_[(size_t)a].cap += bottleneck;
        arcs_[(size_t)sister(a)].cap -= bottleneck;
        const int32_t up = arcs_[(size_t)a].head;
        if (arcs_[(size_t)sister(a)].cap == 0) make_orphan(i);
        i = up;
    }
    for (int32_t i = arcs_[(size_t)middle].head;;) {
        Node& n = nodes_[(size_t)i];
        const int32_t a = n.parent;
        if (a == kTerminal) {
            n.tr += bottleneck;
            if (n.tr == 0) make_orphan(i);
            break;
        }
        arcs_[(size_t)sister(a)].cap += bottleneck;
        arcs_[(size_t)a].cap -= bottleneck;
        const int32_t up = arcs_[(size_t)a].head;
        if (arcs_[(size_t)a].cap == 0) make_orphan(i);
        i = up;
    }
    flow_ += bottleneck;
}

template <typename Cap>
void MaxFlow<Cap>::adopt(int32_t i) {
    Node& n = nodes_[(size_t)i];
    int32_t best = kNone;
    int32_t best_dist = std::numeric_limits<int32_t>::max();
    for (int32_t a = n.first; a >= 0; a = arcs_[(size_t)a].next) {
        if (!(tree_cap(n, a) > 0)) continue;
        const int32_t j = arcs_[(size_t)a].head;
        const Node& nj = nodes_[(size_t)j];
        if (nj.parent == kNone || nj.sink != n.sink) continue;
        // Walk to the root: a terminal makes j a valid parent, an orphan does not.
        int32_t d = 0, k = j;
        bool rooted = false;
        for (;;) {
            const Node& nk = nodes_[(size_t)k];
            if (nk.ts == time_) { d += nk.dist; rooted = true; break; }
            const int32_t pa = nk.parent;
            ++d;
            if (pa == kTerminal) { rooted = true; break; }
            if (pa == kOrphan || pa == kNone) break;
            k = arcs_[(size_t)pa].head;
        }
        if (!rooted) continue;
        if (d < best_dist) { best = a; best_dist = d; }
        // Stamp the path, so later walks this round stop early.
        for (k = j; nodes_[(size_t)k].ts != time_; ) {
            Node& nk = nodes_[(size_t)k];
            nk.ts = time_;
            nk.dist = d--;
            if (nk.parent == kTerminal) break;
            k = arcs_[(size_t)nk.parent].head;
        }
    }
    if (best != kNone) {
        n.parent = best;
        n.ts = time_;
        n.dist = best_dist + 1;
        return;
    }
    for (int32_t a = n.first; a >= 0; a = arcs_[(size_t)a].next) {
        const int32_t j = arcs_[(size_t)a].head;
        Node& nj = nodes_[(size_t)j];
        if (nj.parent == kNone || nj.sink != n.sink) continue;
        if (tree_cap(n, a) > 0) activate(j);
        if (nj.parent != kTerminal && nj.parent != kOrphan && arcs_[(size_t)nj.parent].head == i)
            make_orphan(j);
    }
    n.parent = kNone;
}

}  // namespace meshing
