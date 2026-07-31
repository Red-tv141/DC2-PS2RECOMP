#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>

struct IsoFileInfo
{
    uint32_t lba;
    uint32_t size;
    bool is_dir;
};

class Ps2IsoMount
{
public:
    // Accepts either a disc image FILE (ISO9660/UDF, the original behaviour) or a
    // DIRECTORY holding the extracted disc tree (G449 folder mode). Both present the
    // identical findFile/readSector surface, so no caller needs to know which is live.
    bool open(const std::string& iso_path);
    bool findFile(const std::string& path, IsoFileInfo& out) const;
    bool readSector(uint32_t lba, uint32_t count, void* dst) const;
    void listRoot(std::vector<std::string>& out) const;
    bool isOpen() const;
    bool isFolderMode() const { return m_folderMode; }

private:
    // ---- G449 folder mode --------------------------------------------------
    // Every extracted file is assigned a synthetic, sector-aligned LBA range. That
    // keeps the whole LBA-based call graph (sceCdRead, the DATA.HD2 archive index's
    // DATA.DAT base+offset arithmetic, the FMV streamer's absolute-LBN reads) working
    // byte-for-byte against a directory, with no caller changes.
    struct FolderExtent
    {
        uint32_t lba;      // first synthetic sector
        uint32_t sectors;  // ceil(size / 2048)
        uint64_t size;     // real byte size on disk
        std::string path;  // absolute host path
    };

    bool openFolder(const std::string& dir);
    bool readSectorFolder(uint32_t lba, uint32_t count, void* dst) const;
    const FolderExtent* extentForSector(uint32_t lba) const;
    std::ifstream* streamFor(const FolderExtent& ex) const;

    bool m_folderMode = false;
    std::vector<FolderExtent> m_extents;                 // sorted by lba, non-overlapping
    mutable std::unordered_map<std::string, std::unique_ptr<std::ifstream>> m_folderStreams;
    mutable std::mutex m_folderMutex;

    bool parseIso9660();
    void walkIso9660Dir(uint32_t lba, uint32_t size,
                        const std::string& prefix, bool is_root, int depth);

    bool parseUdf();
    void walkUdfDir(uint32_t fe_lbn, const std::string& prefix,
                    bool is_root, int depth);
    uint32_t udfLbaToSector(uint32_t lbn) const;

    mutable std::ifstream m_file;
    bool m_open = false;

    std::unordered_map<std::string, IsoFileInfo> m_iso9660Files;
    std::unordered_map<std::string, IsoFileInfo> m_udfFiles;

    bool m_udfPresent = false;
    uint32_t m_udfPartitionStart = 0;

    std::vector<std::string> m_rootEntries;
};

// Global singleton — Phase B accesses ISO via this getter.
Ps2IsoMount& getGlobalIsoMount();

// G449: THE single resolution rule for where game data comes from. Every call site that
// used to inline its own hardcoded ISO candidate list now calls this instead, so the data
// source is chosen in exactly one place. Order:
//   1. DC2_DATA_DIR    — an extracted disc folder (what the launcher passes; no ISO needed)
//   2. DC2_ISO_PATH    — an explicit disc image
//   3. the legacy hardcoded candidates, for existing scripts and harnesses
// Idempotent: safe to call from every site; the mount itself is a once-open singleton.
bool dc2OpenGameDataSource();
