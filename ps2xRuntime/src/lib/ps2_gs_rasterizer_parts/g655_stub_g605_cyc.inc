// G655 P17: normal-build contract for the default-off replay-leaf timing census.  This does not
// touch g605_tri_span.inc, which contains shipped sprite/triangle replay optimizations.
enum
{
    kG605LeafTriUntex = 0,
    kG605LeafTriFast,
    kG605LeafTriGeneric,
    kG605LeafSprite,
    kG605LeafLine,
    kG605LeafSpriteUntex,
    kG605LeafSpriteSlow,
    kG605LeafN
};

struct G605CycScope
{
    G605CycScope(uint32_t, int) {}
    void setLeaf(int) {}
    void setPx(uint64_t, uint64_t) {}
    void setInside(uint64_t) {}
    template <typename... T> void setShape(T &&...) {}
    void markLoop() {}
};
