#include "Common.h"
#include "MemoryCard.h"

#include <cstdarg>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOUSER
#include <Windows.h>
#endif

namespace ps2_stubs
{
    namespace
    {
        constexpr int32_t kMcCmdGetInfo = 0x01;
        constexpr int32_t kMcCmdOpen = 0x02;
        constexpr int32_t kMcCmdClose = 0x03;
        constexpr int32_t kMcCmdSeek = 0x04;
        constexpr int32_t kMcCmdRead = 0x05;
        constexpr int32_t kMcCmdWrite = 0x06;
        constexpr int32_t kMcCmdFlush = 0x0A;
        constexpr int32_t kMcCmdMkdir = 0x0B;
        constexpr int32_t kMcCmdChdir = 0x0C;
        constexpr int32_t kMcCmdGetDir = 0x0D;
        constexpr int32_t kMcCmdSetFileInfo = 0x0E;
        constexpr int32_t kMcCmdDelete = 0x0F;
        constexpr int32_t kMcCmdFormat = 0x10;
        constexpr int32_t kMcCmdUnformat = 0x11;
        constexpr int32_t kMcCmdGetEntSpace = 0x12;
        constexpr int32_t kMcCmdRename = 0x13;

        constexpr int32_t kMcResultSucceed = 0;
        constexpr int32_t kMcResultChangedCard = -1;
        constexpr int32_t kMcResultNoFormat = -2;
        constexpr int32_t kMcResultNoEntry = -4;
        constexpr int32_t kMcResultDeniedPermit = -5;
        constexpr int32_t kMcResultNotEmpty = -6;
        constexpr int32_t kMcResultUpLimitHandle = -7;

        constexpr int32_t kMcTypePs2 = 2;
        constexpr int32_t kMcFormatted = 1;
        constexpr int32_t kMcUnformatted = 0;
        constexpr int32_t kMcFreeClusters = 0x2000;
        constexpr size_t kMcMaxPathLen = 1024;
        constexpr size_t kMcMaxOpenFiles = 32;

        constexpr uint16_t kMcAttrReadable = 0x0001;
        constexpr uint16_t kMcAttrWriteable = 0x0002;
        constexpr uint16_t kMcAttrFile = 0x0010;
        constexpr uint16_t kMcAttrSubdir = 0x0020;
        constexpr uint16_t kMcAttrClosed = 0x0080;
        constexpr uint16_t kMcAttrHidden = 0x2000;
        constexpr uint16_t kMcAttrExists = 0x8000;

        constexpr uint32_t kMcFileInfoCreate = 0x01;
        constexpr uint32_t kMcFileInfoModify = 0x02;
        constexpr uint32_t kMcFileInfoAttr = 0x04;
        constexpr uint32_t kMcFileInfoValidMask =
            kMcFileInfoCreate | kMcFileInfoModify | kMcFileInfoAttr;

        struct SceMcStDateTime
        {
            uint8_t Resv2 = 0;
            uint8_t Sec = 0;
            uint8_t Min = 0;
            uint8_t Hour = 0;
            uint8_t Day = 0;
            uint8_t Month = 0;
            uint16_t Year = 0;
        };

        struct SceMcTblGetDir
        {
            SceMcStDateTime _Create{};
            SceMcStDateTime _Modify{};
            uint32_t FileSizeByte = 0;
            uint16_t AttrFile = 0;
            uint16_t Reserve1 = 0;
            uint32_t Reserve2 = 0;
            uint32_t PdaAplNo = 0;
            char EntryName[32]{};
        };

        static_assert(sizeof(SceMcTblGetDir) == 64, "sceMcTblGetDir size mismatch");

        struct McOpenFile
        {
            FILE *file = nullptr;
            int32_t port = 0;
            std::filesystem::path hostPath;
        };

        struct McPortState
        {
            std::string currentDir = "/";
            bool formatted = true;
        };

        // G374: opt-in libmc call trace (`DC2_G374_MC_TRACE=1`). Bounded so a long run
        // cannot flood stderr. Never call getenv per-call — cached in a function-local
        // static (see the getenv hot-path rule from G268).
        bool mcTraceEnabled()
        {
            static const bool s_on = []() {
                const char *v = std::getenv("DC2_G374_MC_TRACE");
                return v != nullptr && *v != '\0' && *v != '0';
            }();
            return s_on;
        }

        void mcTrace(const char *fmt, ...)
        {
            if (!mcTraceEnabled())
            {
                return;
            }

            static std::atomic<uint32_t> s_count{0};
            if (s_count.fetch_add(1u) >= 4096u)
            {
                return;
            }

            std::va_list args;
            va_start(args, fmt);
            std::fprintf(stderr, "[G374:mc] ");
            std::vfprintf(stderr, fmt, args);
            va_end(args);
            std::fputc('\n', stderr);
            std::fflush(stderr);
        }

        // G374 ROOT FIX #2 — argument ABI. These stubs used to fetch the 5th and later
        // arguments off the caller's stack (the o32 convention). The R5900 EE ABI the game
        // is built with passes the first EIGHT integer arguments in registers: $a0-$a3
        // ($4-$7) then $t0-$t3 ($8-$11). Proven from the guest prologues:
        //   sceMcGetInfo @0x00122F20:  `move s5,t0`      -> arg5 (the FORMAT out-pointer)
        //   sceMcGetDir  @0x00123118:  `move s1,t0` / `move s2,t1` -> args 5 and 6
        // Reading arg5 off the stack returned garbage (observed: 0x0000000A), so
        // sceMcGetInfo wrote the "card is formatted" flag to a junk address instead of
        // MC_CARD_INFO[2]. The field stayed 0, and every `info[2] == 0` test in the save
        // menu (CSaveMenuClass::KeyStep, SubGameSaveKey) read "unformatted" and pushed the
        // player into the format prompt. Same class of defect as G373: a hand-written
        // stub's register convention guessed instead of read from the guest prologue.
        uint32_t getArgU32(R5900Context *ctx, int index)
        {
            // 0..3 -> $a0-$a3 ($4-$7); 4..7 -> $t0-$t3 ($8-$11).
            const int reg = (index < 4) ? (4 + index) : (8 + (index - 4));
            return getRegU32(ctx, reg);
        }

        std::mutex g_mcStateMutex;
        int32_t g_mcNextFd = 1;
        int32_t g_mcLastCmd = 0;
        int32_t g_mcLastResult = 0;
        std::unordered_map<int32_t, McOpenFile> g_mcFiles;
        std::array<McPortState, 2> g_mcPorts{};
        int32_t g_cvMcFileCursor = 0;
        constexpr int32_t kCvMcFreeCapacityBytes = 0x01000000;
        constexpr int32_t kCvMcSaveCapacityBytes = 0x00080000;
        constexpr int32_t kCvMcConfigCapacityBytes = 0x00008000;
        constexpr int32_t kCvMcIconCapacityBytes = 0x00004000;

        // G374 ROOT FIX. This gate used to demand `slot == 0`, which rejected EVERY libmc
        // call DC2 makes: the game passes slot = 1 at every call site
        // (`sceMcGetInfo(SlotSelect, 1, ...)`, `sceMcChdir(SlotSelect, 1, ...)`, ... — see
        // `CMemoryCardManager::SearchMcType` @0x002F21E0). With the gate failing,
        // sceMcGetInfo wrote cardType = 0 / format = 0 and reported kMcResultNoEntry, so
        // `McCheckMCPs2` @0x002F5390 (which requires MC_CARD_INFO[1] == 2) answered "no
        // card" and the save UI refused to open.
        //
        // On real hardware the slot argument only selects a multitap sub-slot; mcserv
        // serves the base card for the non-multitap slots the game uses, so accepting any
        // slot on a valid port is the faithful behaviour, not a workaround.
        // Rollback lever: DC2_G374_NO_MC_SLOT=1 restores the old slot == 0 gate.
        bool isValidMcPortSlot(int32_t port, int32_t slot)
        {
            if (port < 0 || port >= static_cast<int32_t>(g_mcPorts.size()))
            {
                return false;
            }

            static const bool s_strictSlot = []() {
                const char *v = std::getenv("DC2_G374_NO_MC_SLOT");
                return v != nullptr && *v != '\0' && *v != '0';
            }();

            return s_strictSlot ? (slot == 0) : true;
        }

// G449: DC2_MEMCARD1_DIR / DC2_MEMCARD2_DIR. Shared with Path.h's getConfiguredMcRoot()
// so both routes to card storage resolve identically — see the file's header comment.
#include "../Helpers/g449_memcard_dirs.inc"

        std::filesystem::path getMcRootPath(int32_t port)
        {
            // A configured folder is authoritative for its slot and is used verbatim: the
            // per-port derivation below (mc0 -> mc1 by string surgery) must NOT be applied
            // to it, or slot 2's explicit folder would be rewritten into a sibling.
            {
                std::filesystem::path overridden;
                if (g449MemoryCardDirOverride(static_cast<int>(port), overridden))
                    return overridden;
            }

            const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
            std::filesystem::path root = paths.mcRoot;
            if (root.empty())
            {
                if (!paths.elfDirectory.empty())
                {
                    root = paths.elfDirectory / "mc0";
                }
                else
                {
                    std::error_code ec;
                    const std::filesystem::path cwd = std::filesystem::current_path(ec);
                    root = ec ? std::filesystem::path("mc0") : (cwd / "mc0");
                }
            }

            root = root.lexically_normal();
            if (port <= 0)
            {
                return root;
            }

            const std::filesystem::path parent = root.parent_path();
            const std::string leaf = root.filename().string();
            const std::string lowerLeaf = toLowerAscii(leaf);
            if (lowerLeaf == "mc0")
            {
                return (parent / "mc1").lexically_normal();
            }
            if (leaf.empty())
            {
                return (root / "mc1").lexically_normal();
            }

            return (parent / (leaf + "_slot" + std::to_string(port))).lexically_normal();
        }

        void ensureMcRootExists(int32_t port)
        {
            std::error_code ec;
            std::filesystem::create_directories(getMcRootPath(port), ec);
        }

        std::vector<std::string> splitMcPathComponents(const std::string &value)
        {
            std::vector<std::string> parts;
            std::string current;
            for (char c : value)
            {
                if (c == '/' || c == '\\')
                {
                    if (!current.empty())
                    {
                        parts.push_back(current);
                        current.clear();
                    }
                }
                else
                {
                    current.push_back(c);
                }
            }

            if (!current.empty())
            {
                parts.push_back(current);
            }

            return parts;
        }

        std::string joinMcPathComponents(const std::vector<std::string> &parts)
        {
            if (parts.empty())
            {
                return "/";
            }

            std::string joined = "/";
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i != 0u)
                {
                    joined.push_back('/');
                }
                joined.append(parts[i]);
            }
            return joined;
        }

        std::string normalizeGuestMcPathLocked(int32_t port, std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            const std::string lower = toLowerAscii(path);
            if (lower.rfind("mc0:", 0) == 0 || lower.rfind("mc1:", 0) == 0)
            {
                path = path.substr(4);
            }

            const bool absolute = !path.empty() && path.front() == '/';
            std::vector<std::string> parts;
            if (!absolute && port >= 0 && port < static_cast<int32_t>(g_mcPorts.size()))
            {
                parts = splitMcPathComponents(g_mcPorts[static_cast<size_t>(port)].currentDir);
            }

            for (const std::string &part : splitMcPathComponents(path))
            {
                if (part.empty() || part == ".")
                {
                    continue;
                }

                if (part == "..")
                {
                    if (!parts.empty())
                    {
                        parts.pop_back();
                    }
                    continue;
                }

                parts.push_back(part);
            }

            return joinMcPathComponents(parts);
        }

        std::filesystem::path guestMcPathToHostPath(int32_t port, const std::string &guestPath)
        {
            std::filesystem::path resolved = getMcRootPath(port);
            if (guestPath.size() > 1u)
            {
                resolved /= std::filesystem::path(guestPath.substr(1));
            }
            return resolved.lexically_normal();
        }

        bool localtimeSafeMc(const std::time_t *value, std::tm *out)
        {
#ifdef _WIN32
            return localtime_s(out, value) == 0;
#else
            return localtime_r(value, out) != nullptr;
#endif
        }

        std::time_t fileTimeToTimeTMc(std::filesystem::file_time_type value)
        {
            const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(systemTime);
        }

        std::filesystem::file_time_type timeTToFileTimeMc(std::time_t value)
        {
            const auto systemTime = std::chrono::system_clock::from_time_t(value);
            return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
                systemTime - std::chrono::system_clock::now() + std::filesystem::file_time_type::clock::now());
        }

        bool readMcDateTime(const SceMcStDateTime &value, std::time_t &out)
        {
            if (value.Year < 1970u || value.Month < 1u || value.Month > 12u ||
                value.Day < 1u || value.Day > 31u || value.Hour > 23u ||
                value.Min > 59u || value.Sec > 60u)
            {
                return false;
            }

            std::tm tm{};
            tm.tm_sec = value.Sec;
            tm.tm_min = value.Min;
            tm.tm_hour = value.Hour;
            tm.tm_mday = value.Day;
            tm.tm_mon = static_cast<int>(value.Month) - 1;
            tm.tm_year = static_cast<int>(value.Year) - 1900;
            tm.tm_isdst = -1;
            out = std::mktime(&tm);
            return out != static_cast<std::time_t>(-1);
        }

#ifdef _WIN32
        std::time_t winFileTimeToTimeTMc(const FILETIME &value)
        {
            ULARGE_INTEGER ticks{};
            ticks.LowPart = value.dwLowDateTime;
            ticks.HighPart = value.dwHighDateTime;
            constexpr uint64_t kWindowsToUnixEpoch100ns = 116444736000000000ULL;
            if (ticks.QuadPart < kWindowsToUnixEpoch100ns)
            {
                return 0;
            }
            return static_cast<std::time_t>((ticks.QuadPart - kWindowsToUnixEpoch100ns) / 10000000ULL);
        }

        FILETIME timeTToWinFileTimeMc(std::time_t value)
        {
            constexpr uint64_t kWindowsToUnixEpoch100ns = 116444736000000000ULL;
            ULARGE_INTEGER ticks{};
            ticks.QuadPart = static_cast<uint64_t>(value) * 10000000ULL + kWindowsToUnixEpoch100ns;
            FILETIME result{};
            result.dwLowDateTime = ticks.LowPart;
            result.dwHighDateTime = ticks.HighPart;
            return result;
        }
#endif

        void readHostMcMetadata(const std::filesystem::path &path,
                                std::time_t fallbackTime,
                                std::time_t &createdTime,
                                std::time_t &modifiedTime,
                                bool &writeable,
                                bool &hidden)
        {
            createdTime = fallbackTime;
            modifiedTime = fallbackTime;
            writeable = true;
            hidden = false;

#ifdef _WIN32
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                createdTime = winFileTimeToTimeTMc(data.ftCreationTime);
                modifiedTime = winFileTimeToTimeTMc(data.ftLastWriteTime);
                writeable = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0;
                hidden = (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
                return;
            }
#endif

            std::error_code ec;
            const auto status = std::filesystem::status(path, ec);
            if (!ec)
            {
                const auto perms = status.permissions();
                writeable = (perms & (std::filesystem::perms::owner_write |
                                      std::filesystem::perms::group_write |
                                      std::filesystem::perms::others_write)) != std::filesystem::perms::none;
            }
            ec.clear();
            const auto lastWrite = std::filesystem::last_write_time(path, ec);
            if (!ec)
            {
                modifiedTime = fileTimeToTimeTMc(lastWrite);
                createdTime = modifiedTime;
            }
        }

        bool setHostMcMetadata(const std::filesystem::path &path,
                               const SceMcTblGetDir &info,
                               uint32_t flags)
        {
            std::time_t createTime = 0;
            std::time_t modifyTime = 0;
            const bool setCreate = (flags & kMcFileInfoCreate) != 0u &&
                                   readMcDateTime(info._Create, createTime);
            const bool setModify = (flags & kMcFileInfoModify) != 0u &&
                                   readMcDateTime(info._Modify, modifyTime);

            if (((flags & kMcFileInfoCreate) != 0u && !setCreate) ||
                ((flags & kMcFileInfoModify) != 0u && !setModify))
            {
                return false;
            }

#ifdef _WIN32
            const DWORD openFlags = std::filesystem::is_directory(path)
                                        ? FILE_FLAG_BACKUP_SEMANTICS
                                        : FILE_ATTRIBUTE_NORMAL;
            HANDLE handle = CreateFileW(path.c_str(),
                                        FILE_WRITE_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        openFlags,
                                        nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            const FILETIME createFileTime = timeTToWinFileTimeMc(createTime);
            const FILETIME modifyFileTime = timeTToWinFileTimeMc(modifyTime);
            const BOOL timeResult =
                (!setCreate && !setModify) ||
                SetFileTime(handle,
                            setCreate ? &createFileTime : nullptr,
                            nullptr,
                            setModify ? &modifyFileTime : nullptr);
            CloseHandle(handle);
            if (!timeResult)
            {
                return false;
            }
#else
            if (setModify)
            {
                std::error_code ec;
                std::filesystem::last_write_time(path, timeTToFileTimeMc(modifyTime), ec);
                if (ec)
                {
                    return false;
                }
            }
#endif

            if ((flags & kMcFileInfoAttr) != 0u)
            {
#ifdef _WIN32
                DWORD attrs = GetFileAttributesW(path.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES)
                {
                    return false;
                }
                if ((info.AttrFile & kMcAttrWriteable) != 0u)
                    attrs &= ~FILE_ATTRIBUTE_READONLY;
                else
                    attrs |= FILE_ATTRIBUTE_READONLY;
                if ((info.AttrFile & kMcAttrHidden) != 0u)
                    attrs |= FILE_ATTRIBUTE_HIDDEN;
                else
                    attrs &= ~FILE_ATTRIBUTE_HIDDEN;
                if (!SetFileAttributesW(path.c_str(), attrs))
                {
                    return false;
                }
#else
                std::error_code ec;
                const auto writePerms = std::filesystem::perms::owner_write |
                                        std::filesystem::perms::group_write |
                                        std::filesystem::perms::others_write;
                const auto operation = (info.AttrFile & kMcAttrWriteable) != 0u
                                           ? std::filesystem::perm_options::add
                                           : std::filesystem::perm_options::remove;
                std::filesystem::permissions(path, writePerms, operation, ec);
                if (ec)
                {
                    return false;
                }
#endif
            }

            return true;
        }

        void writeMcCString(uint8_t *rdram, uint32_t addr, const std::string &value)
        {
            if (addr == 0u)
            {
                return;
            }

            uint8_t *dst = getMemPtr(rdram, addr);
            if (!dst)
            {
                return;
            }

            std::memcpy(dst, value.c_str(), value.size() + 1u);
        }

        void writeMcDateTime(SceMcStDateTime &out, std::time_t value)
        {
            std::tm tm{};
            if (!localtimeSafeMc(&value, &tm))
            {
                std::memset(&out, 0, sizeof(out));
                return;
            }

            out.Resv2 = 0;
            out.Sec = static_cast<uint8_t>(tm.tm_sec);
            out.Min = static_cast<uint8_t>(tm.tm_min);
            out.Hour = static_cast<uint8_t>(tm.tm_hour);
            out.Day = static_cast<uint8_t>(tm.tm_mday);
            out.Month = static_cast<uint8_t>(tm.tm_mon + 1);
            out.Year = static_cast<uint16_t>(tm.tm_year + 1900);
        }

        void fillMcDirTableEntry(SceMcTblGetDir &entry,
                                 const std::string &name,
                                 bool isDirectory,
                                 uint32_t sizeBytes,
                                 std::time_t createdTime,
                                 std::time_t modifiedTime,
                                 bool writeable,
                                 bool hidden)
        {
            std::memset(&entry, 0, sizeof(entry));
            writeMcDateTime(entry._Create, createdTime);
            writeMcDateTime(entry._Modify, modifiedTime);
            entry.FileSizeByte = isDirectory ? 0u : sizeBytes;
            entry.AttrFile = static_cast<uint16_t>(kMcAttrReadable |
                                                   (writeable ? kMcAttrWriteable : 0u) |
                                                   (isDirectory ? kMcAttrSubdir : kMcAttrFile) |
                                                   kMcAttrClosed |
                                                   (hidden ? kMcAttrHidden : 0u) |
                                                   kMcAttrExists);
            std::strncpy(entry.EntryName, name.c_str(), sizeof(entry.EntryName) - 1u);
            entry.EntryName[sizeof(entry.EntryName) - 1u] = '\0';
        }

        bool wildcardMatch(const std::string &pattern, const std::string &value)
        {
            size_t patternPos = 0u;
            size_t valuePos = 0u;
            size_t starPos = std::string::npos;
            size_t matchPos = 0u;

            while (valuePos < value.size())
            {
                if (patternPos < pattern.size() &&
                    (pattern[patternPos] == '?' || pattern[patternPos] == value[valuePos]))
                {
                    ++patternPos;
                    ++valuePos;
                }
                else if (patternPos < pattern.size() && pattern[patternPos] == '*')
                {
                    starPos = patternPos++;
                    matchPos = valuePos;
                }
                else if (starPos != std::string::npos)
                {
                    patternPos = starPos + 1u;
                    valuePos = ++matchPos;
                }
                else
                {
                    return false;
                }
            }

            // G374 ROOT FIX #3. A trailing '?' must also match the END of the name, not
            // just one character. DC2 CREATES its save directories with
            // `"BASCUS-97213dkcl%d"` (slot 0 -> "BASCUS-97213dkcl0", a ONE-digit suffix)
            // but ENUMERATES them with the query `"BASCUS-97213dkcl??"` — see
            // `CMemoryCardManager::MakeDir` @0x002F2550 and the query observed in the
            // sceMcGetDir trace. Under a strict "'?' == exactly one character" matcher the
            // game can never find the saves it just wrote, so the save list came back
            // empty. mcserv's matcher accepts the short name; a strict matcher is what was
            // wrong here, and the game's own create/search pair is the proof.
            while (patternPos < pattern.size() &&
                   (pattern[patternPos] == '*' || pattern[patternPos] == '?'))
            {
                ++patternPos;
            }

            return patternPos == pattern.size();
        }

        void setMcCommandResultLocked(int32_t cmd, int32_t result)
        {
            g_mcLastCmd = cmd;
            g_mcLastResult = result;
        }

        void closeMcFilesLocked()
        {
            for (auto &[fd, openFile] : g_mcFiles)
            {
                if (openFile.file)
                {
                    std::fclose(openFile.file);
                    openFile.file = nullptr;
                }
            }
            g_mcFiles.clear();
        }

        void closeMcFilesForPortLocked(int32_t port)
        {
            for (auto it = g_mcFiles.begin(); it != g_mcFiles.end();)
            {
                if (it->second.port == port)
                {
                    if (it->second.file)
                    {
                        std::fclose(it->second.file);
                    }
                    it = g_mcFiles.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        int32_t allocateMcFdLocked(FILE *file, int32_t port, const std::filesystem::path &hostPath)
        {
            if (!file)
            {
                return kMcResultDeniedPermit;
            }
            if (g_mcFiles.size() >= kMcMaxOpenFiles)
            {
                return kMcResultUpLimitHandle;
            }

            for (int attempt = 0; attempt < 0x10000; ++attempt)
            {
                if (g_mcNextFd <= 0)
                {
                    g_mcNextFd = 1;
                }

                const int32_t fd = g_mcNextFd++;
                if (g_mcFiles.find(fd) != g_mcFiles.end())
                {
                    continue;
                }

                g_mcFiles.emplace(fd, McOpenFile{file, port, hostPath});
                return fd;
            }

            return kMcResultUpLimitHandle;
        }

        FILE *openMcHostFile(const std::filesystem::path &hostPath, uint32_t flags)
        {
            const uint32_t access = flags & PS2_FIO_O_RDWR;
            const bool read = (access == PS2_FIO_O_RDONLY) || (access == PS2_FIO_O_RDWR);
            const bool write = (access == PS2_FIO_O_WRONLY) || (access == PS2_FIO_O_RDWR);
            const bool append = (flags & PS2_FIO_O_APPEND) != 0u;
            const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
            const bool truncate = (flags & PS2_FIO_O_TRUNC) != 0u;

            std::error_code ec;
            const bool exists = std::filesystem::exists(hostPath, ec) && !ec;

            const char *mode = "rb";
            if (read && write)
            {
                if (append)
                {
                    mode = exists ? "a+b" : "w+b";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "w+b";
                }
                else
                {
                    mode = "r+b";
                }
            }
            else if (write)
            {
                if (append)
                {
                    mode = exists ? "ab" : "wb";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "wb";
                }
                else
                {
                    mode = "r+b";
                }
            }

            return std::fopen(hostPath.string().c_str(), mode);
        }
    }

    void sceMcChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceMcChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t pathAddr = getRegU32(ctx, 6);
        const uint32_t currentDirAddr = getRegU32(ctx, 7);
        const std::string requestedDir =
            (pathAddr != 0u) ? readPs2CStringBounded(rdram, pathAddr, kMcMaxPathLen) : std::string{};

        std::string currentDir = "/";
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                currentDir = state.currentDir;
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string resolvedDir =
                        requestedDir.empty() ? state.currentDir : normalizeGuestMcPathLocked(port, requestedDir);
                    const std::filesystem::path hostDir = guestMcPathToHostPath(port, resolvedDir);
                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        state.currentDir = resolvedDir;
                        currentDir = resolvedDir;
                        result = kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdChdir, result);
        }
        mcTrace("Chdir port=%d slot=%d req='%s' cwd='%s' -> %d",
                port, slot, requestedDir.c_str(), currentDir.c_str(), result);

        writeMcCString(rdram, currentDirAddr, currentDir);
        setReturnS32(ctx, 0);
    }

    void sceMcClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end())
            {
                if (!it->second.file || std::fclose(it->second.file) == 0)
                {
                    result = kMcResultSucceed;
                }
                g_mcFiles.erase(it);
            }
            setMcCommandResultLocked(kMcCmdClose, result);
        }
        mcTrace("Close fd=%d -> %d", fd, result);
        setReturnS32(ctx, 0);
    }

    void sceMcDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    if (guestPath != "/")
                    {
                        const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                        std::error_code ec;
                        if (std::filesystem::exists(hostPath, ec) && !ec)
                        {
                            if (std::filesystem::is_directory(hostPath, ec) &&
                                !std::filesystem::is_empty(hostPath, ec))
                            {
                                result = kMcResultNotEmpty;
                            }
                            else if (std::filesystem::remove(hostPath, ec) && !ec)
                            {
                                result = kMcResultSucceed;
                            }
                            else
                            {
                                result = kMcResultDeniedPermit;
                            }
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdDelete, result);
        }
        mcTrace("Delete -> %d", result);
        setReturnS32(ctx, 0);
    }

    void sceMcEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
            }
        }

        // libmc teardown is just a local state reset in this immediate runtime model.
        setReturnS32(ctx, 0);
    }

    void sceMcFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end() && it->second.file)
            {
                result = (std::fflush(it->second.file) == 0) ? kMcResultSucceed : kMcResultDeniedPermit;
            }
            setMcCommandResultLocked(kMcCmdFlush, result);
        }
        mcTrace("Flush fd=%d -> %d", fd, result);
        setReturnS32(ctx, 0);
    }

    void sceMcFormat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                closeMcFilesForPortLocked(port);
                const std::filesystem::path root = getMcRootPath(port);
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::create_directories(root, ec);
                if (!ec)
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    state.currentDir = "/";
                    state.formatted = true;
                    result = kMcResultSucceed;
                }
            }

            setMcCommandResultLocked(kMcCmdFormat, result);
        }
        mcTrace("Format port=%d slot=%d -> %d", port, slot, result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetDir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string rawPath = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const int32_t maxEntries = static_cast<int32_t>(getArgU32(ctx, 4)); // $t0
        const uint32_t tableAddr = getArgU32(ctx, 5);                       // $t1

        std::vector<SceMcTblGetDir> entries;
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string guestQuery =
                        normalizeGuestMcPathLocked(port, rawPath.empty() ? "." : rawPath);
                    const bool hasWildcard =
                        guestQuery.find('*') != std::string::npos || guestQuery.find('?') != std::string::npos;

                    const std::filesystem::path queryRel =
                        (guestQuery.size() > 1u) ? std::filesystem::path(guestQuery.substr(1)) : std::filesystem::path{};

                    std::filesystem::path parentRel;
                    std::string pattern;
                    if (hasWildcard)
                    {
                        parentRel = queryRel.parent_path();
                        pattern = queryRel.filename().string();
                    }
                    else
                    {
                        const std::filesystem::path queryHostPath = guestMcPathToHostPath(port, guestQuery);
                        std::error_code queryEc;
                        if (std::filesystem::exists(queryHostPath, queryEc) && !queryEc &&
                            std::filesystem::is_directory(queryHostPath, queryEc))
                        {
                            parentRel = queryRel;
                            pattern = "*";
                        }
                        else
                        {
                            parentRel = queryRel.parent_path();
                            pattern = queryRel.filename().string();
                        }
                    }

                    if (pattern.empty())
                    {
                        pattern = "*";
                    }

                    std::filesystem::path hostDir = getMcRootPath(port);
                    if (!parentRel.empty())
                    {
                        hostDir /= parentRel;
                    }
                    hostDir = hostDir.lexically_normal();

                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        const std::time_t now = std::time(nullptr);
                        auto appendSpecial = [&](const std::string &name)
                        {
                            if (!wildcardMatch(pattern, name))
                            {
                                return;
                            }
                            SceMcTblGetDir entry{};
                            fillMcDirTableEntry(entry, name, true, 0u, now, now, true, false);
                            entries.push_back(entry);
                        };

                        appendSpecial(".");
                        appendSpecial("..");

                        std::vector<std::filesystem::directory_entry> dirEntries;
                        for (const auto &entry : std::filesystem::directory_iterator(
                                 hostDir, std::filesystem::directory_options::skip_permission_denied, ec))
                        {
                            if (ec)
                            {
                                break;
                            }
                            dirEntries.push_back(entry);
                        }

                        std::sort(dirEntries.begin(), dirEntries.end(),
                                  [](const std::filesystem::directory_entry &lhs,
                                     const std::filesystem::directory_entry &rhs)
                                  {
                                      return toLowerAscii(lhs.path().filename().string()) <
                                             toLowerAscii(rhs.path().filename().string());
                                  });

                        for (const auto &entry : dirEntries)
                        {
                            const std::string name = entry.path().filename().string();
                            if (!wildcardMatch(pattern, name))
                            {
                                continue;
                            }

                            std::error_code entryEc;
                            const bool isDirectory = entry.is_directory(entryEc) && !entryEc;
                            const uint32_t sizeBytes =
                                isDirectory ? 0u : static_cast<uint32_t>(entry.file_size(entryEc));
                            entryEc.clear();
                            const std::time_t fallbackTime = fileTimeToTimeTMc(entry.last_write_time(entryEc));
                            std::time_t createdTime = entryEc ? now : fallbackTime;
                            std::time_t modifiedTime = entryEc ? now : fallbackTime;
                            bool writeable = true;
                            bool hidden = false;
                            readHostMcMetadata(entry.path(),
                                               fallbackTime,
                                               createdTime,
                                               modifiedTime,
                                               writeable,
                                               hidden);
                            SceMcTblGetDir tableEntry{};
                            fillMcDirTableEntry(tableEntry,
                                                name,
                                                isDirectory,
                                                sizeBytes,
                                                createdTime,
                                                modifiedTime,
                                                writeable,
                                                hidden);
                            entries.push_back(tableEntry);
                        }

                        const size_t entryCount =
                            std::min(entries.size(), maxEntries > 0 ? static_cast<size_t>(maxEntries) : 0u);
                        if (entryCount == 0u || tableAddr == 0u)
                        {
                            result = static_cast<int32_t>(entryCount);
                        }
                        else if (uint8_t *dst = getMemPtr(rdram, tableAddr))
                        {
                            std::memcpy(dst, entries.data(), entryCount * sizeof(SceMcTblGetDir));
                            result = static_cast<int32_t>(entryCount);
                        }
                        else
                        {
                            result = kMcResultDeniedPermit;
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdGetDir, result);
        }
        mcTrace("GetDir port=%d slot=%d query='%s' mode=%u max=%d table=0x%08x -> %d",
                port, slot, rawPath.c_str(), static_cast<unsigned>(getRegU32(ctx, 7)),
                maxEntries, tableAddr, result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetEntSpace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1024);
    }

    void sceMcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t typePtr = getRegU32(ctx, 6);
        const uint32_t freePtr = getRegU32(ctx, 7);
        const uint32_t formatPtr = getArgU32(ctx, 4); // $t0 — see getArgU32 above

        int32_t cardType = 0;
        int32_t freeBlocks = 0;
        int32_t format = kMcUnformatted;
        int32_t result = kMcResultNoEntry;

        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                cardType = kMcTypePs2;
                freeBlocks = state.formatted ? kMcFreeClusters : 0;
                format = state.formatted ? kMcFormatted : kMcUnformatted;
                result = state.formatted ? kMcResultSucceed : kMcResultNoFormat;
            }

            setMcCommandResultLocked(kMcCmdGetInfo, result);
        }

        if (typePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, typePtr))
            {
                std::memcpy(out, &cardType, sizeof(cardType));
            }
        }
        if (freePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, freePtr))
            {
                std::memcpy(out, &freeBlocks, sizeof(freeBlocks));
            }
        }
        if (formatPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, formatPtr))
            {
                std::memcpy(out, &format, sizeof(format));
            }
        }

        mcTrace("GetInfo port=%d slot=%d -> type=%d free=%d format=%d result=%d",
                port, slot, cardType, freeBlocks, format, result);

        // G374 diagnostic: the guest passes `&MC_CARD_INFO.format` as the 5th arg
        // (`sceMcGetInfo(port, 1, &info[1], &info[5], &info[2])` in
        // `CMemoryCardManager::SearchMcType`), so the struct base is formatPtr - 8. Dump
        // the whole 8-word record: info[0]=present, [1]=type, [2]=format, [3]=changed,
        // [5]=free, [7]=last sync result. This is what `McCheckMCPs2` and the save menu's
        // "is it formatted" test (`info[2] == 0`) actually read.
        if (mcTraceEnabled() && formatPtr >= 8u)
        {
            if (const uint8_t *base = getMemPtr(rdram, formatPtr - 8u))
            {
                int32_t w[8];
                std::memcpy(w, base, sizeof(w));
                mcTrace("  MC_CARD_INFO@0x%08x = [%d %d %d %d %d %d %d %d]",
                        formatPtr - 8u, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }

        setReturnS32(ctx, 0);
    }

    void sceMcGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceMcInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
                state.formatted = true;
            }
        }
        ensureMcRootExists(0);
        ensureMcRootExists(1);
        setReturnS32(ctx, 0);
    }

    void sceMcMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                    std::error_code ec;
                    if (std::filesystem::exists(hostPath, ec) && !ec)
                    {
                        result = std::filesystem::is_directory(hostPath, ec) ? kMcResultSucceed : kMcResultDeniedPermit;
                    }
                    else if (std::filesystem::create_directory(hostPath, ec) && !ec)
                    {
                        result = kMcResultSucceed;
                    }
                    else
                    {
                        result = std::filesystem::exists(hostPath.parent_path(), ec) && !ec
                                     ? kMcResultDeniedPermit
                                     : kMcResultNoEntry;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdMkdir, result);
        }
        mcTrace("Mkdir port=%d slot=%d path='%s' -> %d", port, slot, path.c_str(), result);
        setReturnS32(ctx, 0);
    }

    void sceMcOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const uint32_t flags = getRegU32(ctx, 7);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                    std::error_code ec;
                    const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
                    const bool exists = std::filesystem::exists(hostPath, ec) && !ec;
                    if (guestPath == "/")
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (exists && std::filesystem::is_directory(hostPath, ec))
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (!exists && !create)
                    {
                        result = kMcResultNoEntry;
                    }
                    else if (!std::filesystem::exists(hostPath.parent_path(), ec) || ec)
                    {
                        result = kMcResultNoEntry;
                    }
                    else
                    {
                        FILE *file = openMcHostFile(hostPath, flags);
                        if (!file)
                        {
                            result = exists ? kMcResultDeniedPermit : kMcResultNoEntry;
                        }
                        else
                        {
                            result = allocateMcFdLocked(file, port, hostPath);
                            if (result < 0)
                            {
                                std::fclose(file);
                            }
                        }
                    }
                }
            }
            setMcCommandResultLocked(kMcCmdOpen, result);
        }
        mcTrace("Open port=%d slot=%d path='%s' flags=0x%x -> %d",
                port, slot, path.c_str(), static_cast<unsigned>(flags), result);
        setReturnS32(ctx, 0);
    }

    void sceMcRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t dstAddr = getRegU32(ctx, 5);
        const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
        uint8_t *dst = (size > 0) ? getMemPtr(rdram, dstAddr) : nullptr;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (size <= 0)
            {
                result = 0;
            }
            else if (it == g_mcFiles.end() || !it->second.file)
            {
                result = kMcResultNoEntry;
            }
            else if (!dst)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                const size_t bytesRead = std::fread(dst, 1u, static_cast<size_t>(size), it->second.file);
                result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesRead);
                if (std::ferror(it->second.file))
                {
                    std::clearerr(it->second.file);
                }
            }

            setMcCommandResultLocked(kMcCmdRead, result);
        }
        mcTrace("Read fd=%d size=%d -> %d", fd, size, result);
        setReturnS32(ctx, 0);
    }

    void sceMcRename(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string oldPath = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const std::string newPath = readPs2CStringBounded(rdram, getRegU32(ctx, 7), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::filesystem::path oldHostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, oldPath));
                    const std::filesystem::path newHostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, newPath));
                    std::error_code ec;
                    if (std::filesystem::exists(oldHostPath, ec) && !ec &&
                        std::filesystem::exists(newHostPath.parent_path(), ec) && !ec)
                    {
                        std::filesystem::rename(oldHostPath, newHostPath, ec);
                        result = ec ? kMcResultDeniedPermit : kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdRename, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t offset = static_cast<int32_t>(getRegU32(ctx, 5));
        const int32_t origin = static_cast<int32_t>(getRegU32(ctx, 6));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end() && it->second.file)
            {
                int whence = SEEK_SET;
                if (origin == PS2_FIO_SEEK_CUR)
                {
                    whence = SEEK_CUR;
                }
                else if (origin == PS2_FIO_SEEK_END)
                {
                    whence = SEEK_END;
                }

                if (std::fseek(it->second.file, offset, whence) == 0)
                {
                    const long position = std::ftell(it->second.file);
                    result = (position >= 0) ? static_cast<int32_t>(position) : kMcResultDeniedPermit;
                }
                else
                {
                    result = kMcResultDeniedPermit;
                }
            }

            setMcCommandResultLocked(kMcCmdSeek, result);
        }
        mcTrace("Seek fd=%d -> %d", fd, result);
        setReturnS32(ctx, 0);
    }

    void sceMcSetFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const uint32_t infoAddr = getRegU32(ctx, 7);
        const uint32_t flags = getArgU32(ctx, 4) & kMcFileInfoValidMask; // $t0
        SceMcTblGetDir info{};
        const uint8_t *infoPtr = getMemPtr(rdram, infoAddr);
        if (infoPtr)
        {
            std::memcpy(&info, infoPtr, sizeof(info));
        }

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::filesystem::path hostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, path));
                    std::error_code ec;
                    if (!infoPtr)
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (std::filesystem::exists(hostPath, ec) && !ec)
                    {
                        result = setHostMcMetadata(hostPath, info, flags)
                                     ? kMcResultSucceed
                                     : kMcResultDeniedPermit;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdSetFileInfo, result);
        }
        mcTrace("SetFileInfo info=0x%08X flags=0x%X -> %d", infoAddr, flags, result);
        setReturnS32(ctx, 0);
    }

    void sceMcSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cmdPtr = getRegU32(ctx, 5);
        const uint32_t resultPtr = getRegU32(ctx, 6);
        int32_t cmd = 0;
        int32_t result = 0;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            cmd = g_mcLastCmd;
            result = g_mcLastResult;
        }

        if (cmdPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, cmdPtr))
            {
                std::memcpy(out, &cmd, sizeof(cmd));
            }
        }
        if (resultPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, resultPtr))
            {
                std::memcpy(out, &result, sizeof(result));
            }
        }

        mcTrace("Sync -> cmd=%d result=%d", cmd, result);

        // 1 = command finished in this runtime's immediate model.
        setReturnS32(ctx, 1);
    }

    void sceMcUnformat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                closeMcFilesForPortLocked(port);
                const std::filesystem::path root = getMcRootPath(port);
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::create_directories(root, ec);
                if (!ec)
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    state.currentDir = "/";
                    state.formatted = false;
                    result = kMcResultSucceed;
                }
            }

            setMcCommandResultLocked(kMcCmdUnformat, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
        const uint8_t *src = (size > 0) ? getConstMemPtr(rdram, srcAddr) : nullptr;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (size <= 0)
            {
                result = 0;
            }
            else if (it == g_mcFiles.end() || !it->second.file)
            {
                result = kMcResultNoEntry;
            }
            else if (!src)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                const size_t bytesWritten = std::fwrite(src, 1u, static_cast<size_t>(size), it->second.file);
                result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesWritten);
                if (!std::ferror(it->second.file))
                {
                    std::fflush(it->second.file);
                }
                else
                {
                    std::clearerr(it->second.file);
                }
            }

            setMcCommandResultLocked(kMcCmdWrite, result);
        }
        mcTrace("Write fd=%d size=%d -> %d", fd, size, result);
        setReturnS32(ctx, 0);
    }

    void mcCallMessageTypeSe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcCheckReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateConfigInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateIconInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateSaveFileInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcDispFileName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispFileNumber(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplayFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfoMesCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowCurSol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowFoundtion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceGetInfoApdx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceIntrReadFixAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceStorePwd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetConfigCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcConfigCapacityBytes);
    }

    void mcGetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_cvMcFileCursor);
    }

    void mcGetFreeCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcFreeCapacityBytes);
    }

    void mcGetIconCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconCapacityBytes);
    }

    void mcGetIconFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconCapacityBytes);
    }

    void mcGetPortSelectDirInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetSaveFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcSaveCapacityBytes);
    }

    void mcGetStringEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t strAddr = getRegU32(ctx, 4);
        const std::string value = readPs2CStringBounded(rdram, runtime, strAddr, 1024);
        setReturnU32(ctx, strAddr + static_cast<uint32_t>(value.size()));
    }

    void mcMoveFileSelectWindowCursor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t delta = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor += delta;
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcNewCreateConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateIcon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSelectFileInfoInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 1);
    }

    void mcSelectSaveFileCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcSetFileSelectWindowCursolInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 0);
    }

    void mcSetStringSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcSetTyepWriteMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcWriteIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }
}
