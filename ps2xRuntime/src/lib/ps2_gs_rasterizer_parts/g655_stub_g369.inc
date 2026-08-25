// G655 P17: normal-build interfaces for the refuted cutscene scene census.
bool g369CensusEnabled() { return false; }
void g369NoteDrawPrim(uint32_t, const GSVertex &, const GSVertex &, const GSVertex &,
                      uint32_t, uint32_t, uint32_t) {}
void g369NoteTriangle(float, float, float, float, float, float,
                      float, float, float, bool) {}
static void g369NoteSprite() {}
static void g369CensusFlush(uint64_t) {}
