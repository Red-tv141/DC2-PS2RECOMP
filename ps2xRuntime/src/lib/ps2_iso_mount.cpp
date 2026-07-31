#include "ps2_iso_mount.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <filesystem>

// ---- constants ---------------------------------------------------------------

static constexpr uint32_t SECTOR_SIZE   = 2048;
static constexpr uint32_t PVD_SECTOR    = 16;
static constexpr uint32_t AVDP_SECTOR   = 256;
static constexpr int      MAX_DIR_DEPTH = 12;

// ---- byte helpers ------------------------------------------------------------

static inline uint16_t r16(const uint8_t* b, uint32_t o)
{
    return static_cast<uint16_t>(b[o]) | (static_cast<uint16_t>(b[o + 1]) << 8);
}

static inline uint32_t r32(const uint8_t* b, uint32_t o)
{
    return static_cast<uint32_t>(b[o])
         | (static_cast<uint32_t>(b[o + 1]) << 8)
         | (static_cast<uint32_t>(b[o + 2]) << 16)
         | (static_cast<uint32_t>(b[o + 3]) << 24);
}

static std::string toUpper(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static std::string stripVersion(std::string name)
{
    const auto sc = name.rfind(';');
    if (sc != std::string::npos)
        name.erase(sc);
    return name;
}

// ---- Ps2IsoMount public API --------------------------------------------------

bool Ps2IsoMount::isOpen() const { return m_open; }

bool Ps2IsoMount::readSector(uint32_t lba, uint32_t count, void* dst) const
{
    if (!m_open) return false;
    if (m_folderMode) return readSectorFolder(lba, count, dst);
    m_file.seekg(static_cast<std::streamoff>(lba) * SECTOR_SIZE, std::ios::beg);
    if (!m_file) return false;
    m_file.read(static_cast<char*>(dst),
                static_cast<std::streamsize>(count) * SECTOR_SIZE);
    return m_file.good() || m_file.eof();
}

bool Ps2IsoMount::open(const std::string& iso_path)
{
    // F55: idempotent. A second open() on the already-mounted singleton used to
    // fail on the open ifstream (failbit) while leaving m_open=true, poisoning
    // every later readSector(). Keep the existing mount instead.
    if (m_open && (m_folderMode || m_file.is_open()))
        return true;

    // G449: a DIRECTORY means the extracted disc tree. Decide before touching ifstream —
    // opening a directory as a file "succeeds" on some runtimes and then fails every read.
    {
        std::error_code ec;
        if (std::filesystem::is_directory(iso_path, ec) && !ec)
            return openFolder(iso_path);
    }

    m_file.clear();
    m_file.open(iso_path, std::ios::binary);
    if (!m_file)
    {
        std::cerr << "[ISO] Cannot open: " << iso_path << std::endl;
        return false;
    }
    m_open = true;

    if (!parseIso9660())
    {
        std::cerr << "[ISO] ISO9660 parse failed: " << iso_path << std::endl;
        m_open = false;
        return false;
    }

    parseUdf(); // optional — failure is non-fatal

    std::cout << "[ISO] Mounted: " << iso_path
              << "  iso9660_files=" << m_iso9660Files.size();
    if (m_udfPresent)
        std::cout << "  udf_files=" << m_udfFiles.size();
    std::cout << std::endl;
    return true;
}

bool Ps2IsoMount::findFile(const std::string& path, IsoFileInfo& out) const
{
    std::string key = toUpper(path);

    auto it = m_iso9660Files.find(key);
    if (it != m_iso9660Files.end())
    {
        out = it->second;
        return true;
    }

    if (m_udfPresent)
    {
        auto uit = m_udfFiles.find(key);
        if (uit != m_udfFiles.end())
        {
            out = uit->second;
            return true;
        }
    }
    return false;
}

void Ps2IsoMount::listRoot(std::vector<std::string>& out) const
{
    out = m_rootEntries;
}

// ---- ISO 9660 ---------------------------------------------------------------
//
// PVD layout (sector 16):
//   [0]       type (1 = Primary)
//   [1-5]     "CD001"
//   [40-71]   volume identifier (32 bytes)
//   [156-189] root directory record (34 bytes)
//
// Directory record layout:
//   [0]      len_dr
//   [1]      extended attribute length
//   [2-5]    LBA of extent (LE)
//   [10-13]  data length (LE)
//   [25]     flags  (0x02 = directory)
//   [32]     len_fi
//   [33+]    file identifier

bool Ps2IsoMount::parseIso9660()
{
    uint8_t sector[SECTOR_SIZE];
    if (!readSector(PVD_SECTOR, 1, sector)) return false;

    if (sector[0] != 1) return false;
    if (std::memcmp(&sector[1], "CD001", 5) != 0) return false;

    char volId[33] = {};
    std::memcpy(volId, &sector[40], 32);
    // rtrim trailing spaces
    for (int i = 31; i >= 0 && volId[i] == ' '; --i) volId[i] = '\0';
    std::cout << "[ISO] Volume ID: " << volId << std::endl;

    const uint32_t rootLba  = r32(sector, 158); // offset 156+2
    const uint32_t rootSize = r32(sector, 166); // offset 156+10

    if (rootLba == 0 || rootSize == 0) return false;

    walkIso9660Dir(rootLba, rootSize, "", /*is_root=*/true, 0);
    return !m_iso9660Files.empty() || !m_rootEntries.empty();
}

void Ps2IsoMount::walkIso9660Dir(uint32_t lba, uint32_t size,
                                  const std::string& prefix,
                                  bool is_root, int depth)
{
    if (depth > MAX_DIR_DEPTH) return;
    if (size == 0) return;

    const uint32_t sectors = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    std::vector<uint8_t> buf(static_cast<size_t>(sectors) * SECTOR_SIZE, 0);
    if (!readSector(lba, sectors, buf.data())) return;

    uint32_t offset = 0;
    while (offset < size)
    {
        const uint8_t* dr = buf.data() + offset;
        const uint8_t  len_dr = dr[0];

        if (len_dr == 0)
        {
            // padding to next sector boundary
            const uint32_t nextSector = ((offset / SECTOR_SIZE) + 1) * SECTOR_SIZE;
            if (nextSector >= size) break;
            offset = nextSector;
            continue;
        }

        if (offset + len_dr > size) break;

        const uint8_t len_fi = dr[32];
        if (len_fi == 0) { offset += len_dr; continue; }

        // skip . and ..
        if (len_fi == 1 && (dr[33] == 0x00 || dr[33] == 0x01))
        {
            offset += len_dr;
            continue;
        }

        const uint32_t fileLba  = r32(dr, 2);
        const uint32_t fileSize = r32(dr, 10);
        const bool     is_dir   = (dr[25] & 0x02) != 0;

        std::string name(reinterpret_cast<const char*>(&dr[33]), len_fi);
        name = toUpper(stripVersion(name));

        const std::string fullPath = prefix + "/" + name;
        m_iso9660Files[fullPath] = IsoFileInfo{fileLba, fileSize, is_dir};

        if (is_root)
            m_rootEntries.push_back(name);

        if (is_dir && fileLba != 0)
            walkIso9660Dir(fileLba, fileSize, fullPath, false, depth + 1);

        offset += len_dr;
    }
}

// ---- UDF (ECMA-167) ----------------------------------------------------------
//
// Sector 256 = Anchor Volume Descriptor Pointer (tag_id = 2)
//   [16-19]  Main VDS extent length
//   [20-23]  Main VDS extent location (sector)
//
// Main VDS sectors:
//   tag_id 5 = PartitionDescriptor  → partition_starting_location @ 188
//   tag_id 6 = LogicalVolumeDescriptor
//              LogicalVolumeContentsUse (LongAD, 16 bytes) @ 248
//                [248-251] extent_length
//                [252-255] FSD LBN (partition-relative)
//                [256-257] partition reference
//   tag_id 8/0 = TerminatingDescriptor → stop scan
//
// FileSetDescriptor (tag_id = 256), sector = partition_start + fsd_lbn:
//   Root Directory ICB (LongAD, 16 bytes) @ 400
//     [404-407] root FileEntry LBN (partition-relative)
//
// FileEntry (tag_id = 260) / ExtendedFileEntry (tag_id = 261):
//   IcbTag @ 16  →  file_type @ 27  (4 = directory, 5 = file)
//                    flags      @ 34  (bits 0-2 = alloc type)
//   InformationLength (8 bytes) @ 56  (low 32 bits = file size)
//   EALength (4 bytes) @ 168
//   ADLength (4 bytes) @ 172
//   EAs start @ 176
//   ADs start @ 176 + EALength
//     short alloc (type 0): 4-byte extent_len, 4-byte lbn
//     long  alloc (type 1): 4-byte extent_len, 4-byte lbn, 2-byte part, 6-byte impl
//
// FileIdentifierDescriptor (tag_id = 257) inside directory data:
//   File Characteristics (1) @ 16 : bit1=directory, bit2=deleted, bit3=parent
//   Length of FI (1)          @ 17
//   ICB LongAD (16)           @ 18  →  lbn @ 22
//   Length of IU (2)          @ 34
//   Implementation Use (L_IU) @ 36
//   File Identifier (L_FI)    @ 36 + L_IU   (CS0 compressed unicode)
//   Padding to 4-byte boundary
//   Total = ((36 + L_IU + L_FI) + 3) & ~3

uint32_t Ps2IsoMount::udfLbaToSector(uint32_t lbn) const
{
    return m_udfPartitionStart + lbn;
}

bool Ps2IsoMount::parseUdf()
{
    uint8_t sector[SECTOR_SIZE];

    // --- AVDP ---
    if (!readSector(AVDP_SECTOR, 1, sector)) return false;
    if (r16(sector, 0) != 2) return false; // not AVDP

    const uint32_t vdsLen = r32(sector, 16);
    const uint32_t vdsLoc = r32(sector, 20);
    if (vdsLen == 0 || vdsLoc == 0) return false;

    // --- Main VDS scan ---
    const uint32_t vdsSectors = (vdsLen + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t fsdLbn = 0;
    bool gotPartition = false;
    bool gotFsd       = false;

    for (uint32_t i = 0; i < vdsSectors && i < 32; ++i)
    {
        if (!readSector(vdsLoc + i, 1, sector)) break;
        const uint16_t tid = r16(sector, 0);

        if (tid == 5) // PartitionDescriptor
        {
            m_udfPartitionStart = r32(sector, 188);
            gotPartition = true;
        }
        else if (tid == 6) // LogicalVolumeDescriptor
        {
            // LogicalVolumeContentsUse LongAD @ 248
            // extent_length @ 248, lbn @ 252, part_ref @ 256
            fsdLbn = r32(sector, 252);
            gotFsd = true;
        }
        else if (tid == 8 || tid == 0)
        {
            break; // TerminatingDescriptor
        }
    }

    if (!gotPartition || !gotFsd) return false;

    // --- FileSetDescriptor ---
    const uint32_t fsdSector = udfLbaToSector(fsdLbn);
    if (!readSector(fsdSector, 1, sector)) return false;
    if (r16(sector, 0) != 256) return false; // not FSD

    // Root Directory ICB LongAD @ 400 → lbn @ 404
    const uint32_t rootFeLbn = r32(sector, 404);

    m_udfPresent = true;
    walkUdfDir(rootFeLbn, "", /*is_root=*/true, 0);
    return true;
}

void Ps2IsoMount::walkUdfDir(uint32_t fe_lbn, const std::string& prefix,
                              bool /*is_root*/, int depth)
{
    if (depth > MAX_DIR_DEPTH) return;

    uint8_t feBuf[SECTOR_SIZE];
    const uint32_t feSector = udfLbaToSector(fe_lbn);
    if (!readSector(feSector, 1, feBuf)) return;

    const uint16_t feTid = r16(feBuf, 0);
    if (feTid != 260 && feTid != 261) return; // must be FileEntry or ExtendedFE

    // file_type @ 27 (ICB tag starts @ 16, file_type is byte 11 of IcbTag)
    if (feBuf[27] != 4) return; // must be a directory

    const uint16_t icbFlags  = r16(feBuf, 34); // IcbTag.flags
    const uint8_t  allocType = icbFlags & 0x3;

    const uint32_t eaLen = r32(feBuf, 168);
    const uint32_t adLen = r32(feBuf, 172);
    const uint32_t adOff = 176 + eaLen;

    if (adOff + adLen > SECTOR_SIZE) return; // AD table overflows — skip

    // Collect directory content bytes
    std::vector<uint8_t> dirContent;
    dirContent.reserve(SECTOR_SIZE * 4);

    uint32_t pos = adOff;
    const uint32_t adEnd = adOff + adLen;

    while (pos < adEnd)
    {
        uint32_t extLen = 0;
        uint32_t extLbn = 0;

        if (allocType == 0 && pos + 8 <= adEnd) // short alloc
        {
            extLen = r32(feBuf, pos) & 0x3FFFFFFFu;
            extLbn = r32(feBuf, pos + 4);
            pos += 8;
        }
        else if (allocType == 1 && pos + 16 <= adEnd) // long alloc
        {
            extLen = r32(feBuf, pos) & 0x3FFFFFFFu;
            extLbn = r32(feBuf, pos + 4);
            pos += 16;
        }
        else
        {
            break;
        }

        if (extLen == 0) continue;
        const uint32_t extSectors = (extLen + SECTOR_SIZE - 1) / SECTOR_SIZE;
        const size_t   oldSz      = dirContent.size();
        dirContent.resize(oldSz + static_cast<size_t>(extSectors) * SECTOR_SIZE, 0);
        readSector(udfLbaToSector(extLbn), extSectors, dirContent.data() + oldSz);
    }

    // Parse File Identifier Descriptors
    uint32_t fidOff = 0;
    while (fidOff + 36 <= dirContent.size())
    {
        const uint8_t* fid = dirContent.data() + fidOff;

        if (r16(fid, 0) != 257) // not FID — try to recover by advancing 4 bytes
        {
            fidOff += 4;
            continue;
        }

        const uint8_t  fc    = fid[16]; // File Characteristics
        const uint8_t  lenFi = fid[17];
        const uint16_t lenIu = r16(fid, 34);

        // Compute total FID length
        const uint32_t fidLen = ((36u + lenIu + lenFi) + 3u) & ~3u;
        if (fidLen == 0 || fidOff + fidLen > dirContent.size()) break;

        const bool isDeleted = (fc & 0x04) != 0;
        const bool isParent  = (fc & 0x08) != 0;
        const bool isDir     = (fc & 0x02) != 0;

        if (!isDeleted && !isParent && lenFi > 0)
        {
            // ICB LongAD @ 18 → child lbn @ 22
            const uint32_t childLbn = r32(fid, 22);

            // Decode CS0 file identifier
            const uint8_t* fiData = fid + 36 + lenIu;
            std::string name;
            if (lenFi > 1)
            {
                const uint8_t compId = fiData[0];
                if (compId == 8) // 8-bit chars
                {
                    name.assign(reinterpret_cast<const char*>(fiData + 1),
                                lenFi - 1);
                }
                else if (compId == 16) // 16-bit BE chars
                {
                    for (uint8_t j = 1; j + 1 < lenFi; j += 2)
                    {
                        const uint16_t ch = (static_cast<uint16_t>(fiData[j]) << 8)
                                          | fiData[j + 1];
                        name += (ch < 128) ? static_cast<char>(ch) : '?';
                    }
                }
            }

            if (!name.empty())
            {
                name = toUpper(name);
                const std::string fullPath = prefix + "/" + name;

                // Read child FileEntry to get file size and data LBA
                const uint32_t childSector = udfLbaToSector(childLbn);
                uint8_t        cBuf[SECTOR_SIZE];
                IsoFileInfo    fi{childSector, 0u, isDir};

                if (readSector(childSector, 1, cBuf))
                {
                    const uint16_t cTid = r16(cBuf, 0);
                    if (cTid == 260 || cTid == 261)
                    {
                        fi.size = r32(cBuf, 56); // InformationLength low 32

                        // For files, resolve actual data LBA from first short AD
                        if (!isDir)
                        {
                            const uint32_t cEaLen = r32(cBuf, 168);
                            const uint32_t cAdOff = 176 + cEaLen;
                            const uint16_t cFlags = r16(cBuf, 34);
                            const uint8_t  cAlloc = cFlags & 0x3;
                            if (cAlloc == 0 && cAdOff + 8 <= SECTOR_SIZE)
                                fi.lba = udfLbaToSector(r32(cBuf, cAdOff + 4));
                            else if (cAlloc == 1 && cAdOff + 16 <= SECTOR_SIZE)
                                fi.lba = udfLbaToSector(r32(cBuf, cAdOff + 4));
                        }
                    }
                }

                m_udfFiles[fullPath] = fi;

                if (isDir)
                    walkUdfDir(childLbn, fullPath, false, depth + 1);
            }
        }

        fidOff += fidLen;
    }
}

// ---- global singleton -------------------------------------------------------

Ps2IsoMount& getGlobalIsoMount()
{
    static Ps2IsoMount s_iso;
    return s_iso;
}

// ---- G449 folder mode -------------------------------------------------------
//
// Mount an EXTRACTED disc tree (the launcher's DATA folder) behind the identical
// findFile/readSector surface the ISO path exposes. The whole runtime addresses disc
// content by LBA -- sceCdRead/sceCdStRead take absolute LBNs, and the DATA.HD2 archive
// index resolves entries as (DATA.DAT's LBA) + (byte offset within DATA.DAT). So the
// folder cannot simply serve files by name: it must present a SECTOR ADDRESS SPACE.
//
// Each file therefore gets a contiguous, sector-aligned synthetic LBA range, allocated in
// a stable order (sorted by path, so the layout is identical on every run and across
// machines). readSector then maps a sector back to (file, byte offset). Because each file
// is contiguous, base+offset arithmetic inside DATA.DAT lands exactly where it does on the
// real disc, and no caller changes at all.
//
// Reads past a file's end (the last sector of a non-sector-multiple file, which the guest
// requests routinely) zero-fill, matching how the real medium reads a padded final sector.

static constexpr uint32_t kFolderLbaBase = 32; // clear of the PVD/AVDP region (16, 256 are ISO's)

bool Ps2IsoMount::openFolder(const std::string& dir)
{
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (it->is_regular_file(ec) && !ec)
            files.push_back(it->path());
    }
    if (files.empty())
    {
        std::cerr << "[DATA] No files under folder: " << dir << std::endl;
        return false;
    }

    // Deterministic layout: identical synthetic LBAs on every run and every machine.
    std::sort(files.begin(), files.end());

    const std::filesystem::path root(dir);
    uint32_t nextLba = kFolderLbaBase;
    m_extents.clear();
    m_extents.reserve(files.size());
    m_iso9660Files.clear();
    m_rootEntries.clear();

    for (const auto& f : files)
    {
        const uintmax_t sz = std::filesystem::file_size(f, ec);
        if (ec) { ec.clear(); continue; }

        const uint32_t sectors =
            static_cast<uint32_t>((static_cast<uint64_t>(sz) + SECTOR_SIZE - 1) / SECTOR_SIZE);

        FolderExtent ex;
        ex.lba = nextLba;
        ex.sectors = (sectors > 0u) ? sectors : 1u; // a 0-byte file still owns one sector
        ex.size = static_cast<uint64_t>(sz);
        ex.path = f.string();
        m_extents.push_back(ex);

        // Key form must match the ISO path exactly: "/DATA.HD2", "/MOVIE/RUSH.PSS".
        // findFile() upper-cases its argument, so store upper-cased keys here too.
        std::string rel = std::filesystem::relative(f, root, ec).generic_string();
        if (ec) { ec.clear(); rel = f.filename().generic_string(); }
        const std::string key = toUpper("/" + rel);

        IsoFileInfo info{};
        info.lba = ex.lba;
        info.size = static_cast<uint32_t>(std::min<uint64_t>(ex.size, 0xFFFFFFFFull));
        info.is_dir = false;
        m_iso9660Files[key] = info;

        if (rel.find('/') == std::string::npos)
            m_rootEntries.push_back(toUpper(rel));

        nextLba += ex.sectors;
    }

    // Directory entries, so a findFile() on a folder answers is_dir like ISO9660 does.
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_directory(ec) || ec) continue;
        std::string rel = std::filesystem::relative(it->path(), root, ec).generic_string();
        if (ec) { ec.clear(); continue; }
        IsoFileInfo info{};
        info.lba = 0; info.size = 0; info.is_dir = true;
        m_iso9660Files[toUpper("/" + rel)] = info;
        if (rel.find('/') == std::string::npos)
            m_rootEntries.push_back(toUpper(rel));
    }

    if (m_extents.empty())
    {
        std::cerr << "[DATA] No readable files under folder: " << dir << std::endl;
        return false;
    }

    m_folderMode = true;
    m_udfPresent = false;
    m_open = true;
    std::cout << "[DATA] Mounted folder: " << dir
              << "  files=" << m_extents.size()
              << "  sectors=" << (nextLba - kFolderLbaBase) << std::endl;
    return true;
}

const Ps2IsoMount::FolderExtent* Ps2IsoMount::extentForSector(uint32_t lba) const
{
    // m_extents is sorted by lba and non-overlapping by construction.
    auto it = std::upper_bound(m_extents.begin(), m_extents.end(), lba,
                               [](uint32_t v, const FolderExtent& e) { return v < e.lba; });
    if (it == m_extents.begin()) return nullptr;
    --it;
    if (lba < it->lba || lba >= it->lba + it->sectors) return nullptr;
    return &(*it);
}

std::ifstream* Ps2IsoMount::streamFor(const FolderExtent& ex) const
{
    auto it = m_folderStreams.find(ex.path);
    if (it != m_folderStreams.end())
        return it->second.get();
    auto s = std::make_unique<std::ifstream>(ex.path, std::ios::binary);
    if (!*s) return nullptr;
    std::ifstream* raw = s.get();
    m_folderStreams.emplace(ex.path, std::move(s));
    return raw;
}

bool Ps2IsoMount::readSectorFolder(uint32_t lba, uint32_t count, void* dst) const
{
    // Unlike the ISO path (one ifstream, single-threaded by assumption), folder mode keeps
    // a per-file handle cache that several guest threads can reach, so serialise it.
    std::lock_guard<std::mutex> lk(m_folderMutex);

    uint8_t* out = static_cast<uint8_t*>(dst);
    for (uint32_t i = 0; i < count; ++i, out += SECTOR_SIZE)
    {
        const FolderExtent* ex = extentForSector(lba + i);
        if (ex == nullptr)
        {
            // Gap or out-of-range sector: the real medium would return padding here.
            std::memset(out, 0, SECTOR_SIZE);
            continue;
        }
        std::ifstream* s = streamFor(*ex);
        if (s == nullptr)
            return false;

        const uint64_t off = static_cast<uint64_t>(lba + i - ex->lba) * SECTOR_SIZE;
        uint64_t avail = (off < ex->size) ? (ex->size - off) : 0ull;
        if (avail > SECTOR_SIZE) avail = SECTOR_SIZE;

        if (avail < SECTOR_SIZE)
            std::memset(out + avail, 0, SECTOR_SIZE - static_cast<size_t>(avail));
        if (avail == 0)
            continue;

        s->clear();
        s->seekg(static_cast<std::streamoff>(off), std::ios::beg);
        if (!*s) return false;
        s->read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(avail));
        if (!(s->good() || s->eof())) return false;
    }
    return true;
}

// ---- G449 single data-source resolution rule --------------------------------

bool dc2OpenGameDataSource()
{
    Ps2IsoMount& mount = getGlobalIsoMount();
    if (mount.isOpen())
        return true;

    if (const char* dataDir = std::getenv("DC2_DATA_DIR"))
    {
        if (*dataDir && mount.open(dataDir))
            return true;
        std::cerr << "[data] DC2_DATA_DIR set but not mountable: " << dataDir << std::endl;
    }
    if (const char* isoPath = std::getenv("DC2_ISO_PATH"))
    {
        if (*isoPath && mount.open(isoPath))
            return true;
        std::cerr << "[data] DC2_ISO_PATH set but not mountable: " << isoPath << std::endl;
    }

    // Legacy candidates, so every existing harness and script keeps working untouched.
    const char* candidates[] = {
        "DATA",
        "D:/ps2r/dc2/Dark Cloud 2 (USA) (v2.00).iso",
        "Dark Cloud 2 (USA) (v2.00).iso",
    };
    for (const char* p : candidates)
    {
        if (mount.open(p))
            return true;
    }
    std::cerr << "[data] no game data source opened (set DC2_DATA_DIR or DC2_ISO_PATH)\n";
    return false;
}
