// ============================================================================
// G449 — CUSTOM MEMORY-CARD FOLDERS (DC2_MEMCARD1_DIR / DC2_MEMCARD2_DIR).
//
// The launcher lets a user place each memory card wherever they like and already exported
// both variables; the runner ignored them and always derived the card root from the ELF
// directory (or the cwd).
//
// THIS FILE IS INCLUDED BY TWO INDEPENDENT RESOLVERS, deliberately:
//   * Kernel/Stubs/MemoryCard.cpp  -> getMcRootPath(port), the sceMc* API DC2 actually
//     saves and loads through (G374)
//   * Kernel/Syscalls/Helpers/Path.h -> getConfiguredMcRoot(), which backs "mc0:"-prefixed
//     FileIO paths
// They are separate code paths to the same storage. If only one honoured the override, a
// save could be written to the custom folder and looked for in the default one (or the
// reverse) — so the override has to be a single shared rule, applied by both.
//
// Port 0 is slot 1 (DC2_MEMCARD1_DIR), port 1 is slot 2 (DC2_MEMCARD2_DIR), matching the
// launcher's MemoryCard1 / MemoryCard2 naming. Unset or empty => the caller keeps its
// existing derivation untouched, so every script and default layout is unaffected.
//
// Included inside an anonymous namespace in both TUs, so each gets its own internal copy —
// no linkage question, and the cached getenv is per-TU and read once.
// ============================================================================

bool g449MemoryCardDirOverride(int port, std::filesystem::path &out)
{
    auto cached = [](const char *name) -> std::string {
        const char *v = std::getenv(name);
        return (v != nullptr && *v != '\0') ? std::string(v) : std::string();
    };
    static const std::string s_slot1 = cached("DC2_MEMCARD1_DIR");
    static const std::string s_slot2 = cached("DC2_MEMCARD2_DIR");

    const std::string &configured = (port <= 0) ? s_slot1 : s_slot2;
    if (configured.empty())
        return false;

    out = std::filesystem::path(configured).lexically_normal();
    return true;
}
