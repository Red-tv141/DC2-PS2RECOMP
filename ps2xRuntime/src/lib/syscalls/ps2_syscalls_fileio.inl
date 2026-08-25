// ===== G657 P1: 10 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.
// Phase B — ISO-backed file descriptors.
// Forward declarations of helpers defined in ps2_runtime.cpp (same namespace).
bool isoFindFileForFio(const char *path, uint32_t *lbaOut, uint32_t *sizeOut);
bool isoReadSectorForFio(uint32_t lba, uint32_t count, void *dst);

namespace
{
    constexpr uint32_t kIsoSectorSize = 2048u;
    static const bool kFioTrace = true;

    struct IsoFdEntry
    {
        uint32_t lba;
        uint32_t size;        // logical size visible to caller
        uint64_t cursor;      // logical cursor 0..size
        uint64_t baseOffset;  // byte offset within the underlying ISO file (0 for plain ISO files,
                              // entry offset within DATA.DAT for Phase F6 archive fds)
    };

    std::unordered_map<int, IsoFdEntry> g_isoFds;
    std::mutex g_isoFdMutex;

    static int allocateIsoFd(uint32_t lba, uint32_t size)
    {
        int fd;
        {
            std::lock_guard<std::mutex> fdLock(g_fd_mutex);
            fd = g_nextFd++;
        }
        {
            std::lock_guard<std::mutex> isoLock(g_isoFdMutex);
            g_isoFds[fd] = IsoFdEntry{lba, size, 0u, 0u};
        }
        return fd;
    }

    static int allocateArchiveFd(uint32_t datLba, uint32_t offsetInDat, uint32_t size)
    {
        int fd;
        {
            std::lock_guard<std::mutex> fdLock(g_fd_mutex);
            fd = g_nextFd++;
        }
        {
            std::lock_guard<std::mutex> isoLock(g_isoFdMutex);
            g_isoFds[fd] = IsoFdEntry{datLba, size, 0u, static_cast<uint64_t>(offsetInDat)};
        }
        return fd;
    }

    // ---- Phase F6: DATA.HD2 / DATA.DAT archive index ----
    struct DataArchiveEntry
    {
        uint32_t offsetInDat;
        uint32_t size;
    };

    static std::unordered_map<std::string, DataArchiveEntry> g_dataArchive;
    static std::once_flag g_dataArchiveLoaded;
    static uint32_t g_datDatLba = 0;
    static uint32_t g_datDatSize = 0;

    static std::string archiveNormalize(const std::string &path)
    {
        std::string s = path;
        if (!s.empty() && (s.front() == '/' || s.front() == '\\'))
            s.erase(s.begin());
        for (auto &c : s)
        {
            if (c == '\\') c = '/';
            else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    static void loadDataArchiveIndex()
    {
        uint32_t hd2Lba = 0, hd2Size = 0;
        if (!isoFindFileForFio("/DATA.HD2", &hd2Lba, &hd2Size))
        {
            std::cerr << "[Phase F6] DATA.HD2 not found in ISO\n";
            return;
        }
        if (!isoFindFileForFio("/DATA.DAT", &g_datDatLba, &g_datDatSize))
        {
            std::cerr << "[Phase F6] DATA.DAT not found in ISO\n";
            return;
        }

        const uint32_t sectorCount = (hd2Size + kIsoSectorSize - 1) / kIsoSectorSize;
        std::vector<uint8_t> buf(static_cast<size_t>(sectorCount) * kIsoSectorSize, 0);
        for (uint32_t i = 0; i < sectorCount; ++i)
        {
            if (!isoReadSectorForFio(hd2Lba + i, 1u, buf.data() + i * kIsoSectorSize))
            {
                std::cerr << "[Phase F6] failed to read DATA.HD2 sector " << i << "\n";
                return;
            }
        }
        buf.resize(hd2Size);

        // Entries are 32 bytes; first entry's nameOffset doubles as the entry-table end.
        // Stop when nameOffset is out of range or non-monotonic.
        auto rd32 = [&](size_t off) -> uint32_t {
            return static_cast<uint32_t>(buf[off]) |
                   (static_cast<uint32_t>(buf[off + 1]) << 8) |
                   (static_cast<uint32_t>(buf[off + 2]) << 16) |
                   (static_cast<uint32_t>(buf[off + 3]) << 24);
        };

        if (buf.size() < 32) return;
        const uint32_t firstNameOff = rd32(0);
        if (firstNameOff < 32 || firstNameOff > buf.size()) return;
        const uint32_t entryCount = firstNameOff / 32u;

        size_t added = 0;
        for (uint32_t i = 0; i < entryCount; ++i)
        {
            const size_t e = static_cast<size_t>(i) * 32u;
            const uint32_t nameOff = rd32(e + 0x00);
            const uint32_t offDat  = rd32(e + 0x10);
            const uint32_t size    = rd32(e + 0x14);
            if (nameOff < firstNameOff || nameOff >= buf.size()) continue;

            // Read NUL-terminated name from name table
            size_t end = nameOff;
            while (end < buf.size() && buf[end] != 0) ++end;
            std::string raw(reinterpret_cast<const char *>(&buf[nameOff]), end - nameOff);
            if (raw.empty()) continue;

            std::string key = archiveNormalize(raw);
            g_dataArchive.emplace(std::move(key), DataArchiveEntry{offDat, size});
            ++added;
        }

        std::cout << "[Phase F6] loaded DATA archive index: " << added
                  << " entries (DAT lba=0x" << std::hex << g_datDatLba
                  << " size=0x" << g_datDatSize << std::dec << ")\n";
    }

    static bool dataArchiveLookup(const std::string &isoPath,
                                  uint32_t *datLbaOut,
                                  uint32_t *offsetOut,
                                  uint32_t *sizeOut)
    {
        std::call_once(g_dataArchiveLoaded, loadDataArchiveIndex);
        if (g_datDatLba == 0 || g_dataArchive.empty()) return false;

        const std::string key = archiveNormalize(isoPath);
        auto it = g_dataArchive.find(key);
        if (it == g_dataArchive.end()) return false;

        *datLbaOut  = g_datDatLba;
        *offsetOut  = it->second.offsetInDat;
        *sizeOut    = it->second.size;
        return true;
    }

    // Normalize cdrom0:\FOO\BAR;1  →  /FOO/BAR  (uppercase, no ;N suffix)
    // Returns empty string for paths that are not cdrom/host prefixed.
    static std::string extractIsoPath(const char *ps2Path)
    {
        if (!ps2Path || !*ps2Path)
            return {};

        std::string p(ps2Path);
        std::string lower(p.size(), '\0');
        std::transform(p.begin(), p.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        std::size_t prefixLen = 0;
        if (lower.rfind("cdrom0:", 0) == 0)       prefixLen = 7;
        else if (lower.rfind("cdrom:", 0) == 0)   prefixLen = 6;
        else if (lower.rfind("cdfs:", 0) == 0)    prefixLen = 5;
        else if (lower.rfind("host0:", 0) == 0)   prefixLen = 6;
        else if (lower.rfind("host:", 0) == 0)    prefixLen = 5;

        std::string suffix = p.substr(prefixLen);

        // Backslash → forward slash
        std::replace(suffix.begin(), suffix.end(), '\\', '/');

        // Strip ;N version suffix
        const auto sc = suffix.rfind(';');
        if (sc != std::string::npos)
        {
            bool allDigits = (sc + 1 < suffix.size());
            for (std::size_t i = sc + 1; allDigits && i < suffix.size(); ++i)
                allDigits = std::isdigit(static_cast<unsigned char>(suffix[i])) != 0;
            if (allDigits)
                suffix.erase(sc);
        }

        // Trim leading slashes then uppercase
        while (!suffix.empty() && suffix.front() == '/')
            suffix.erase(suffix.begin());
        std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        return suffix.empty() ? std::string{} : "/" + suffix;
    }
}

static int allocatePs2Fd(FILE *file)
{
    if (!file)
        return -1;

    std::lock_guard<std::mutex> lock(g_fd_mutex);
    int fd = g_nextFd++;
    g_fileDescriptors[fd] = file;
    return fd;
}

static FILE *getHostFile(int ps2Fd)
{
    std::lock_guard<std::mutex> lock(g_fd_mutex);
    auto it = g_fileDescriptors.find(ps2Fd);
    if (it != g_fileDescriptors.end())
    {
        return it->second;
    }
    return nullptr;
}

static void releasePs2Fd(int ps2Fd)
{
    std::lock_guard<std::mutex> lock(g_fd_mutex);
    g_fileDescriptors.erase(ps2Fd);
}

struct VagAccumEntry
{
    std::vector<uint8_t> data;
    uint32_t firstBufAddr = 0;
};
static std::unordered_map<int, VagAccumEntry> g_vagAccum;
static std::mutex g_vagAccumMutex;
static constexpr size_t kVagAccumMaxBytes = 16 * 1024 * 1024;

static const char *translateFioMode(int ps2Flags)
{
    bool read = (ps2Flags & PS2_FIO_O_RDONLY) || (ps2Flags & PS2_FIO_O_RDWR);
    bool write = (ps2Flags & PS2_FIO_O_WRONLY) || (ps2Flags & PS2_FIO_O_RDWR);
    bool append = (ps2Flags & PS2_FIO_O_APPEND);
    bool create = (ps2Flags & PS2_FIO_O_CREAT);
    bool truncate = (ps2Flags & PS2_FIO_O_TRUNC);

    if (read && write)
    {
        if (create && truncate)
            return "w+b";
        if (create)
            return "a+b";
        return "r+b";
    }
    else if (write)
    {
        if (append)
            return "ab";
        if (create && truncate)
            return "wb";
        if (create)
            return "wx";
        return "r+b";
    }
    else if (read)
    {
        return "rb";
    }
    return "rb";
}










