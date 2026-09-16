#pragma once
#include <vector>
#include <memory>

struct AABB {
    float cx, cy;  // center
    float hw, hh;  // half-extents

    // Inclusive on all four edges. Entity positions are clamped to exactly
    // [0, WORLD_W] x [0, WORLD_H], so a half-open test would drop everything
    // sitting on the far world edge. Uniqueness of insertion is guaranteed by
    // centre-based child routing (see QuadTree::childIndex) rather than by the
    // containment test.
    bool contains(float x, float y) const;
    bool intersects(const AABB& o)  const;
};

struct QTEntry {
    float x, y;
    int   index;
};

class QuadTree {
public:
    explicit QuadTree(AABB bounds, int depth = 0);

    void clear();
    void insert(const QTEntry& entry);

    // Unfile the entry for `index`, which must be given the same (x, y) it was
    // inserted with -- routing is by position, so a stale coordinate descends
    // into the wrong quadrant and finds nothing. Returns false if absent.
    //
    // Repulsion moves entities in place while it iterates, so without this the
    // tree describes where the swarm was at the top of the tick and quietly
    // drops neighbours that moved into range partway through it.
    bool remove(int index, float x, float y);

    void query(const AABB& range, std::vector<int>& out) const;

private:
    void subdivide();

    // Index of the single child quadrant that owns (x, y): NW=0 NE=1 SW=2 SE=3.
    // Routing by centre comparison means a point on a shared boundary lands in
    // exactly one child, so it can never be counted twice during a query.
    int  childIndex(float x, float y) const;

    AABB                       bounds_;
    int                        depth_;
    std::vector<QTEntry>       entries_;
    std::unique_ptr<QuadTree>  children_[4];  // NW NE SW SE
    bool                       divided_ = false;

    static constexpr int MAX_ENTRIES = 8;
    static constexpr int MAX_DEPTH   = 6;
};
