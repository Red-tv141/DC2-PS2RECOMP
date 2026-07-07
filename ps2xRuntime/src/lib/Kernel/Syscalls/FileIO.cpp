#include "Common.h"
#include "FileIO.h"
#include "ps2_iso_mount.h"

#include <cstdlib>

namespace ps2_syscalls
{
    // ============================================================================
    // PHASE F17 — ported from dead src/lib/syscalls/ps2_syscalls_fileio.inl.
    // ISO9660 + DATA.DAT archive lookup. Wraps the Ps2IsoMount singleton from
    // src/lib/ps2_iso_mount.cpp (added to CMake in F17).
    // ============================================================================
    namespace
    {
        constexpr uint32_t kIsoSectorSize = 2048u;
        static const bool kFioTrace = false;

        // F55: bounded ISO-fd diagnostics, quiet by default.
        static bool f55FioTraceEnabled()
        {
            static const bool enabled = []() {
                const char *v = std::getenv("DC2_TRACE_F55");
                return v && *v && *v != '0';
            }();
            return enabled;
        }

        struct IsoFdEntry
        {
            uint32_t lba;
            uint32_t size;
            uint64_t cursor;
            uint64_t baseOffset; // F6 archive fds use this; plain ISO fds = 0
        };

        static std::unordered_map<int, IsoFdEntry> g_isoFds;
        static std::mutex g_isoFdMutex;

        // Lazy ISO mount. DC2-specific path; later phases should make this configurable.
        static std::once_flag g_isoMountOpened;
        static void ensureIsoOpen()
        {
            std::call_once(g_isoMountOpened, []() {
                const char *isoCandidates[] = {
                    "D:/ps2r/dc2/Dark Cloud 2 (USA) (v2.00).iso",
                    "Dark Cloud 2 (USA) (v2.00).iso",
                };
                for (const char *p : isoCandidates)
                {
                    if (getGlobalIsoMount().open(p))
                        return;
                }
                std::cerr << "[data] ISO mount: no candidate path opened\n";
            });
        }

        static bool isoFindFileForFio(const char *path, uint32_t *lbaOut, uint32_t *sizeOut)
        {
            ensureIsoOpen();
            if (!getGlobalIsoMount().isOpen())
                return false;
            IsoFileInfo info{};
            if (!getGlobalIsoMount().findFile(path, info))
                return false;
            *lbaOut = info.lba;
            *sizeOut = info.size;
            return true;
        }

        static bool isoReadSectorForFio(uint32_t lba, uint32_t count, void *dst)
        {
            ensureIsoOpen();
            return getGlobalIsoMount().readSector(lba, count, dst);
        }

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

        // ---- F6 DATA.HD2 / DATA.DAT archive index ----
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
                std::cerr << "[data] DATA.HD2 not found in ISO\n";
                return;
            }
            if (!isoFindFileForFio("/DATA.DAT", &g_datDatLba, &g_datDatSize))
            {
                std::cerr << "[data] DATA.DAT not found in ISO\n";
                return;
            }

            const uint32_t sectorCount = (hd2Size + kIsoSectorSize - 1) / kIsoSectorSize;
            std::vector<uint8_t> buf(static_cast<size_t>(sectorCount) * kIsoSectorSize, 0);
            for (uint32_t i = 0; i < sectorCount; ++i)
            {
                if (!isoReadSectorForFio(hd2Lba + i, 1u, buf.data() + i * kIsoSectorSize))
                {
                    std::cerr << "[data] failed to read DATA.HD2 sector " << i << "\n";
                    return;
                }
            }
            buf.resize(hd2Size);

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

                size_t end = nameOff;
                while (end < buf.size() && buf[end] != 0) ++end;
                std::string raw(reinterpret_cast<const char *>(&buf[nameOff]), end - nameOff);
                if (raw.empty()) continue;

                std::string key = archiveNormalize(raw);
                g_dataArchive.emplace(std::move(key), DataArchiveEntry{offDat, size});
                ++added;
            }

            std::cout << "[data] loaded DATA archive index: " << added
                      << " entries (DAT lba=0x" << std::hex << g_datDatLba << std::dec << ")\n";
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
            if (it == g_dataArchive.end())
            {
                std::string alias = key;
                const std::size_t slash = alias.find_last_of('/');
                const std::size_t dot = alias.find_last_of('.');
                if (alias.rfind("map/", 0) == 0 &&
                    dot != std::string::npos &&
                    (slash == std::string::npos || dot > slash + 2) &&
                    alias[dot - 2] == '_' &&
                    alias[dot - 1] == '2')
                {
                    alias.erase(dot - 2, 2);
                    it = g_dataArchive.find(alias);
                    if (it != g_dataArchive.end() && std::getenv("DC2_TRACE_F50_7"))
                    {
                        std::cout << "[F50.7:arc-alias] '" << key
                                  << "' -> '" << alias << "'\n";
                    }
                }
            }
            if (it == g_dataArchive.end()) return false;

            *datLbaOut  = g_datDatLba;
            *offsetOut  = it->second.offsetInDat;
            *sizeOut    = it->second.size;
            return true;
        }

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
            std::replace(suffix.begin(), suffix.end(), '\\', '/');

            const auto sc = suffix.rfind(';');
            if (sc != std::string::npos)
            {
                bool allDigits = (sc + 1 < suffix.size());
                for (std::size_t i = sc + 1; allDigits && i < suffix.size(); ++i)
                    allDigits = std::isdigit(static_cast<unsigned char>(suffix[i])) != 0;
                if (allDigits)
                    suffix.erase(sc);
            }

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

    void fioOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        int flags = (int)getRegU32(ctx, 5);    // $a1 (PS2 FIO flags)

        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioOpen error: Invalid path address" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioOpen error: Failed to translate path '" << ps2Path << "'" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        const char *mode = translateFioMode(flags);
        RUNTIME_LOG("fioOpen: '" << hostPath << "' flags=0x" << std::hex << flags << std::dec << " mode='" << mode << "'");

        FILE *fp = ::fopen(hostPath.c_str(), mode);
        if (!fp)
        {
            // PHASE F17 — ISO9660 + DATA.DAT archive fallback for cdrom/host paths.
            const std::string isoPath = extractIsoPath(ps2Path);
            if (!isoPath.empty())
            {
                uint32_t lba = 0, sz = 0;
                if (isoFindFileForFio(isoPath.c_str(), &lba, &sz))
                {
                    const int isoFd = allocateIsoFd(lba, sz);
                    if (kFioTrace)
                        std::cout << "[FIO:iso] sceOpen(\"" << ps2Path << "\") = " << isoFd << std::endl;
                    if (f55FioTraceEnabled())
                    {
                        static uint32_t s_n = 0;
                        if (++s_n <= 16u)
                            std::fprintf(stderr, "[F55:fio-open] iso '%s' fd=%d lba=0x%x size=%u\n",
                                         isoPath.c_str(), isoFd, lba, sz);
                    }
                    setReturnS32(ctx, isoFd);
                    return;
                }

                uint32_t arcLba = 0, arcOff = 0, arcSz = 0;
                if (dataArchiveLookup(isoPath, &arcLba, &arcOff, &arcSz))
                {
                    const int arcFd = allocateArchiveFd(arcLba, arcOff, arcSz);
                    if (kFioTrace)
                        std::cout << "[FIO:arc] '" << isoPath << "' fd=" << arcFd << std::endl;
                    setReturnS32(ctx, arcFd);
                    return;
                }
            }
            std::cerr << "fioOpen error: fopen failed for '" << hostPath << "': " << strerror(errno) << std::endl;
            setReturnS32(ctx, -1); // e.g., -ENOENT, -EACCES
            return;
        }

        int ps2Fd = allocatePs2Fd(fp);
        if (ps2Fd < 0)
        {
            std::cerr << "fioOpen error: Failed to allocate PS2 file descriptor" << std::endl;
            ::fclose(fp);
            setReturnS32(ctx, -1); // e.g., -EMFILE
            return;
        }

        // returns the PS2 file descriptor
        setReturnS32(ctx, ps2Fd);
    }

    void fioClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int ps2Fd = (int)getRegU32(ctx, 4);

        // PHASE F17 — ISO/archive fd fast-path
        {
            std::lock_guard<std::mutex> lock(g_isoFdMutex);
            if (g_isoFds.erase(ps2Fd))
            {
                setReturnS32(ctx, 0);
                return;
            }
        }

        FILE *fp = getHostFile(ps2Fd);
        if (!fp)
        {
            std::cerr << "fioClose warning: Invalid PS2 file descriptor " << ps2Fd << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        int ret = ::fclose(fp);
        releasePs2Fd(ps2Fd);

        {
            std::lock_guard<std::mutex> lock(g_vagAccumMutex);
            auto it = g_vagAccum.find(ps2Fd);
            if (it != g_vagAccum.end())
            {
                VagAccumEntry &e = it->second;
                if (e.data.size() >= 48)
                {
                    const uint32_t magic = (static_cast<uint32_t>(e.data[0]) << 24) |
                                           (static_cast<uint32_t>(e.data[1]) << 16) |
                                           (static_cast<uint32_t>(e.data[2]) << 8) |
                                           static_cast<uint32_t>(e.data[3]);
                    const uint32_t magicLE = (static_cast<uint32_t>(e.data[3]) << 24) |
                                             (static_cast<uint32_t>(e.data[2]) << 16) |
                                             (static_cast<uint32_t>(e.data[1]) << 8) |
                                             static_cast<uint32_t>(e.data[0]);
                    if (magic == 0x56414770u || magicLE == 0x56414770u)
                    {
                        if (runtime)
                            runtime->audioBackend().onVagTransferFromBuffer(
                                e.data.data(), static_cast<uint32_t>(e.data.size()),
                                e.firstBufAddr ? e.firstBufAddr : 0u);
                    }
                }
                g_vagAccum.erase(it);
            }
        }

        setReturnS32(ctx, ret == 0 ? 0 : -1);
    }

    void fioRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int ps2Fd = (int)getRegU32(ctx, 4);   // $a0
        uint32_t bufAddr = getRegU32(ctx, 5); // $a1
        size_t size = getRegU32(ctx, 6);      // $a2

        // PHASE F17 — ISO/archive fd fast-path
        {
            std::lock_guard<std::mutex> isoLock(g_isoFdMutex);
            auto it = g_isoFds.find(ps2Fd);
            if (it != g_isoFds.end())
            {
                IsoFdEntry &e = it->second;
                uint8_t *hbuf = getMemPtr(rdram, bufAddr);
                if (!hbuf)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
                const uint64_t remaining = static_cast<uint64_t>(e.size) - e.cursor;
                const size_t toRead = static_cast<size_t>(
                    std::min<uint64_t>(static_cast<uint64_t>(size), remaining));
                if (toRead == 0)
                {
                    setReturnS32(ctx, 0);
                    return;
                }

                size_t bytesRead = 0;
                std::vector<uint8_t> sectorBuf(kIsoSectorSize);
                const uint64_t physical = e.baseOffset + e.cursor;
                uint32_t sector = e.lba + static_cast<uint32_t>(physical / kIsoSectorSize);
                uint32_t secOff = static_cast<uint32_t>(physical % kIsoSectorSize);

                while (bytesRead < toRead)
                {
                    if (!isoReadSectorForFio(sector, 1u, sectorBuf.data()))
                        break;
                    const size_t avail = kIsoSectorSize - secOff;
                    const size_t chunk = std::min(avail, toRead - bytesRead);
                    std::memcpy(hbuf + bytesRead, sectorBuf.data() + secOff, chunk);
                    bytesRead += chunk;
                    ++sector;
                    secOff = 0u;
                }

                e.cursor += bytesRead;
                if (f55FioTraceEnabled())
                {
                    static uint32_t s_n = 0;
                    if (++s_n <= 16u)
                        std::fprintf(stderr, "[F55:fio-read] fd=%d buf=0x%x size=%zu -> %zu (lba=0x%x cur=%llu)\n",
                                     ps2Fd, bufAddr, size, bytesRead, e.lba,
                                     static_cast<unsigned long long>(e.cursor));
                }
                setReturnS32(ctx, static_cast<int32_t>(bytesRead));
                return;
            }
        }

        uint8_t *hostBuf = getMemPtr(rdram, bufAddr);
        FILE *fp = getHostFile(ps2Fd);

        if (!hostBuf)
        {
            std::cerr << "fioRead error: Invalid buffer address for fd " << ps2Fd << std::endl;
            setReturnS32(ctx, -1); // -EFAULT
            return;
        }
        if (!fp)
        {
            std::cerr << "fioRead error: Invalid file descriptor " << ps2Fd << std::endl;
            setReturnS32(ctx, -1); // -EBADF
            return;
        }
        if (size == 0)
        {
            setReturnS32(ctx, 0); // Read 0 bytes
            return;
        }

        size_t bytesRead = 0;
        {
            std::lock_guard<std::mutex> lock(g_sys_fd_mutex);
            bytesRead = fread(hostBuf, 1, size, fp);
        }

        if (bytesRead < size && ferror(fp))
        {
            std::cerr << "fioRead error: fread failed for fd " << ps2Fd << ": " << strerror(errno) << std::endl;
            clearerr(fp);
            setReturnS32(ctx, -1);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_vagAccumMutex);
            auto it = g_vagAccum.find(ps2Fd);
            if (it != g_vagAccum.end())
            {
                VagAccumEntry &e = it->second;
                if (e.data.size() + bytesRead <= kVagAccumMaxBytes)
                    e.data.insert(e.data.end(), hostBuf, hostBuf + bytesRead);
            }
            else if (bytesRead >= 4)
            {
                const uint32_t magic = (static_cast<uint32_t>(hostBuf[0]) << 24) |
                                       (static_cast<uint32_t>(hostBuf[1]) << 16) |
                                       (static_cast<uint32_t>(hostBuf[2]) << 8) |
                                       static_cast<uint32_t>(hostBuf[3]);
                const uint32_t magicLE = (static_cast<uint32_t>(hostBuf[3]) << 24) |
                                         (static_cast<uint32_t>(hostBuf[2]) << 16) |
                                         (static_cast<uint32_t>(hostBuf[1]) << 8) |
                                         static_cast<uint32_t>(hostBuf[0]);
                if (magic == 0x56414770u || magicLE == 0x56414770u)
                {
                    VagAccumEntry &e = g_vagAccum[ps2Fd];
                    e.firstBufAddr = bufAddr;
                    if (bytesRead <= kVagAccumMaxBytes)
                        e.data.assign(hostBuf, hostBuf + bytesRead);
                }
            }
        }

        setReturnS32(ctx, (int32_t)bytesRead);
    }

    void fioWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int ps2Fd = (int)getRegU32(ctx, 4);   // $a0
        uint32_t bufAddr = getRegU32(ctx, 5); // $a1
        size_t size = getRegU32(ctx, 6);      // $a2

        const uint8_t *hostBuf = getConstMemPtr(rdram, bufAddr);
        if (!hostBuf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        FILE *fp = getHostFile(ps2Fd);
        if (!fp)
        {
            setReturnS32(ctx, -1); // -EFAULT
            return;
        }

        if (size == 0)
        {
            setReturnS32(ctx, 0); // Wrote 0 bytes
            return;
        }

        size_t bytesWritten = 0;
        {
            std::lock_guard<std::mutex> lock(g_sys_fd_mutex);
            bytesWritten = ::fwrite(hostBuf, 1, size, fp);
            if (bytesWritten < size && ferror(fp))
            {
                clearerr(fp);
                setReturnS32(ctx, -1); // -EIO, -ENOSPC etc.
                return;
            }
        }

        // returns number of bytes written
        setReturnS32(ctx, (int32_t)bytesWritten);
    }

    void fioLseek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int ps2Fd = (int)getRegU32(ctx, 4);  // $a0
        int32_t offset = getRegU32(ctx, 5);  // $a1 (PS2 seems to use 32-bit offset here commonly)
        int whence = (int)getRegU32(ctx, 6); // $a2 (PS2 FIO_SEEK constants)

        // PHASE F17 — ISO/archive fd fast-path
        {
            std::lock_guard<std::mutex> lock(g_isoFdMutex);
            auto it = g_isoFds.find(ps2Fd);
            if (it != g_isoFds.end())
            {
                IsoFdEntry &e = it->second;
                int64_t newPos;
                switch (whence)
                {
                case PS2_FIO_SEEK_SET: newPos = static_cast<int64_t>(offset); break;
                case PS2_FIO_SEEK_CUR: newPos = static_cast<int64_t>(e.cursor) + offset; break;
                case PS2_FIO_SEEK_END: newPos = static_cast<int64_t>(e.size) + offset; break;
                default: setReturnS32(ctx, -1); return;
                }
                if (newPos < 0) newPos = 0;
                if (newPos > static_cast<int64_t>(e.size)) newPos = static_cast<int64_t>(e.size);
                e.cursor = static_cast<uint64_t>(newPos);
                if (f55FioTraceEnabled())
                {
                    static uint32_t s_n = 0;
                    if (++s_n <= 16u)
                        std::fprintf(stderr, "[F55:fio-lseek] fd=%d off=%d whence=%d -> %lld\n",
                                     ps2Fd, offset, whence, static_cast<long long>(newPos));
                }
                setReturnS32(ctx, static_cast<int32_t>(newPos));
                return;
            }
        }

        FILE *fp = getHostFile(ps2Fd);
        if (!fp)
        {
            std::cerr << "fioLseek error: Invalid file descriptor " << ps2Fd << std::endl;
            setReturnS32(ctx, -1); // -EBADF
            return;
        }

        int hostWhence;
        switch (whence)
        {
        case PS2_FIO_SEEK_SET:
            hostWhence = SEEK_SET;
            break;
        case PS2_FIO_SEEK_CUR:
            hostWhence = SEEK_CUR;
            break;
        case PS2_FIO_SEEK_END:
            hostWhence = SEEK_END;
            break;
        default:
            std::cerr << "fioLseek error: Invalid whence value " << whence << " for fd " << ps2Fd << std::endl;
            setReturnS32(ctx, -1); // -EINVAL
            return;
        }

        if (::fseek(fp, static_cast<long>(offset), hostWhence) != 0)
        {
            std::cerr << "fioLseek error: fseek failed for fd " << ps2Fd << ": " << strerror(errno) << std::endl;
            setReturnS32(ctx, -1); // Return error code
            return;
        }

        long newPos = ::ftell(fp);
        if (newPos < 0)
        {
            std::cerr << "fioLseek error: ftell failed after fseek for fd " << ps2Fd << ": " << strerror(errno) << std::endl;
            setReturnS32(ctx, -1);
        }
        else
        {
            if (newPos > 0xFFFFFFFFL)
            {
                std::cerr << "fioLseek warning: New position exceeds 32-bit for fd " << ps2Fd << std::endl;
                setReturnS32(ctx, -1);
            }
            else
            {
                setReturnS32(ctx, (int32_t)newPos);
            }
        }
    }

    void fioMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        // int mode = (int)getRegU32(ctx, 5);  // $a1 - ignored on host

        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioMkdir error: Invalid path address" << std::endl;
            setReturnS32(ctx, -1); // -EFAULT
            return;
        }
        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioMkdir error: Failed to translate path '" << ps2Path << "'" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }
        std::error_code ec;
        bool success = std::filesystem::create_directory(hostPath, ec);

        if (!success && ec)
        {
            std::cerr << "fioMkdir error: create_directory failed for '" << hostPath
                      << "': " << ec.message() << std::endl;
            setReturnS32(ctx, -1);
        }
        else
        {
            RUNTIME_LOG("fioMkdir: Created directory '" << hostPath << "'");
            setReturnS32(ctx, 0); // Success
        }
    }

    void fioChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioChdir error: Invalid path address" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioChdir error: Failed to translate path '" << ps2Path << "'" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::error_code ec;
        std::filesystem::current_path(hostPath, ec);

        if (ec)
        {
            std::cerr << "fioChdir error: current_path failed for '" << hostPath
                      << "': " << ec.message() << std::endl;
            setReturnS32(ctx, -1);
        }
        else
        {
            RUNTIME_LOG("fioChdir: Changed directory to '" << hostPath << "'");
            setReturnS32(ctx, 0); // Success
        }
    }

    void fioRmdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioRmdir error: Invalid path address" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }
        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioRmdir error: Failed to translate path '" << ps2Path << "'" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::error_code ec;
        bool success = std::filesystem::remove(hostPath, ec);

        if (!success || ec)
        {
            std::cerr << "fioRmdir error: remove failed for '" << hostPath
                      << "': " << ec.message() << std::endl;
            setReturnS32(ctx, -1);
        }
        else
        {
            RUNTIME_LOG("fioRmdir: Removed directory '" << hostPath << "'");
            setReturnS32(ctx, 0); // Success
        }
    }

    void fioGetstat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // we wont implement this for now.
        uint32_t pathAddr = getRegU32(ctx, 4);    // $a0
        uint32_t statBufAddr = getRegU32(ctx, 5); // $a1

        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        uint8_t *ps2StatBuf = getMemPtr(rdram, statBufAddr);

        if (!ps2Path)
        {
            std::cerr << "fioGetstat error: Invalid path addr" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }
        if (!ps2StatBuf)
        {
            std::cerr << "fioGetstat error: Invalid buffer addr" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioGetstat error: Bad path translate" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        setReturnS32(ctx, -1);
    }

    void fioRemove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        const char *ps2Path = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        if (!ps2Path)
        {
            std::cerr << "fioRemove error: Invalid path" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::string hostPath = translatePs2Path(ps2Path);
        if (hostPath.empty())
        {
            std::cerr << "fioRemove error: Path translate fail" << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::error_code ec;
        bool success = std::filesystem::remove(hostPath, ec);

        if (!success || ec)
        {
            std::cerr << "fioRemove error: remove failed for '" << hostPath
                      << "': " << ec.message() << std::endl;
            setReturnS32(ctx, -1);
        }
        else
        {
            RUNTIME_LOG("fioRemove: Removed file '" << hostPath << "'");
            setReturnS32(ctx, 0); // Success
        }
    }
}
