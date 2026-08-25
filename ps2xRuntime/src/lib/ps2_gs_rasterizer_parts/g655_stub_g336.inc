// G655 P17: normal-build interfaces for the refuted boundary-runway census.
static constexpr bool g336RunwayOn() { return false; }
template <typename... T> static void g336NotePublish(T &&...) {}
template <typename... T> static void g336NotePublishZ(T &&...) {}
template <typename... T> static void g336NoteConsumer(T &&...) {}

// Match rasterizer_g336_runway.inc's scope bridge: the arbiter calls these from another TU.
} // file-level anonymous namespace
void g336_boundary_begin() {}
void g336_boundary_end() {}
namespace
{
