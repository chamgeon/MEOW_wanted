#include "../include/QuadTree.h"
#include <cmath>
#include <algorithm>

bool AABB::contains(float x, float y) const {
    return x >= cx - hw && x <= cx + hw &&
           y >= cy - hh && y <= cy + hh;
}

bool AABB::intersects(const AABB& o) const {
    return std::abs(cx - o.cx) <= hw + o.hw &&
           std::abs(cy - o.cy) <= hh + o.hh;
}

QuadTree::QuadTree(AABB bounds, int depth)
    : bounds_(bounds), depth_(depth) {}

void QuadTree::clear() {
    entries_.clear();
    divided_ = false;
    for (auto& c : children_) c.reset();
}

int QuadTree::childIndex(float x, float y) const {
    const int east  = (x >= bounds_.cx) ? 1 : 0;
    const int south = (y >= bounds_.cy) ? 1 : 0;
    return south * 2 + east;  // NW=0 NE=1 SW=2 SE=3
}

void QuadTree::insert(const QTEntry& entry) {
    if (!bounds_.contains(entry.x, entry.y)) return;

    if (!divided_) {
        // A leaf at MAX_DEPTH cannot subdivide any further, so it has to absorb
        // everything that lands in it. Without this the entry used to be handed
        // to four null children and silently vanish -- which is exactly what
        // happens in this simulation, because every enemy chases the player and
        // the swarm collapses into one deep cell.
        if ((int)entries_.size() < MAX_ENTRIES || depth_ >= MAX_DEPTH) {
            entries_.push_back(entry);
            return;
        }
        subdivide();
    }

    // Exactly one child owns this point.
    children_[childIndex(entry.x, entry.y)]->insert(entry);
}

bool QuadTree::remove(int index, float x, float y) {
    if (!bounds_.contains(x, y)) return false;

    // Mirror insert()'s routing exactly: a divided node holds nothing itself,
    // and an undivided one holds everything that reached it.
    if (!divided_) {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [index](const QTEntry& e) { return e.index == index; });
        if (it == entries_.end()) return false;
        entries_.erase(it);
        return true;
    }

    return children_[childIndex(x, y)]->remove(index, x, y);
}

void QuadTree::query(const AABB& range, std::vector<int>& out) const {
    if (!bounds_.intersects(range)) return;

    for (const auto& e : entries_)
        if (range.contains(e.x, e.y)) out.push_back(e.index);

    if (divided_)
        for (const auto& child : children_)
            child->query(range, out);
}

void QuadTree::subdivide() {
    const float qw = bounds_.hw * 0.5f;
    const float qh = bounds_.hh * 0.5f;
    const float cx = bounds_.cx, cy = bounds_.cy;

    children_[0] = std::make_unique<QuadTree>(AABB{cx - qw, cy - qh, qw, qh}, depth_ + 1); // NW
    children_[1] = std::make_unique<QuadTree>(AABB{cx + qw, cy - qh, qw, qh}, depth_ + 1); // NE
    children_[2] = std::make_unique<QuadTree>(AABB{cx - qw, cy + qh, qw, qh}, depth_ + 1); // SW
    children_[3] = std::make_unique<QuadTree>(AABB{cx + qw, cy + qh, qw, qh}, depth_ + 1); // SE

    divided_ = true;

    // Push the existing payload down, again routing each point to one child.
    for (const auto& e : entries_)
        children_[childIndex(e.x, e.y)]->insert(e);
    entries_.clear();
}
