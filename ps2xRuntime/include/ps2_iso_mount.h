#pragma once
#include <cstdint>
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
    bool open(const std::string& iso_path);
    bool findFile(const std::string& path, IsoFileInfo& out) const;
    bool readSector(uint32_t lba, uint32_t count, void* dst) const;
    void listRoot(std::vector<std::string>& out) const;
    bool isOpen() const;

private:
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
