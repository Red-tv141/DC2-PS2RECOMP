#include "ps2_runtime.h"
#include "register_functions.h"
#include "games_database.h"
#include "lib/ps2_runtime_parts/dc2_logger.inc"
#include "lib/ps2_runtime_parts/dc2_crash_reporter.inc"

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>

namespace
{
    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            return std::filesystem::path(argv[1]);
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
    dc2_log::init("logs");
    dc2_log::setCurrentThreadName("MainThread");
    dc2_crash::installCrashReporter();

    try
    {
        dc2_crash::checkTestCrashTrigger();

        std::filesystem::path pathObj = getExecutablePath(argc, argv);

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        std::string windowTitle = "PS2-Recomp | ";
        const char *gameName = getGameName(normalizedId);

#if !defined(PLATFORM_VITA)
        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
#endif
        {
            windowTitle += elfName;
        }

        dc2_log::StartupConfig startupCfg{};
        startupCfg.gameTitle = gameName ? gameName : "Dark Cloud 2";
        startupCfg.elfName = elfName.c_str();
#if defined(_DEBUG)
        startupCfg.buildType = "Debug";
#else
        startupCfg.buildType = "Release";
#endif
#if defined(_MSC_VER)
        startupCfg.compiler = "MSVC (x64)";
#elif defined(__clang__)
        startupCfg.compiler = "Clang (x64)";
#elif defined(__GNUC__)
        startupCfg.compiler = "GCC (x64)";
#endif
        startupCfg.resolutionW = 640;
        startupCfg.resolutionH = 448;
        startupCfg.isFullscreen = false;
        startupCfg.isVSync = true;
        startupCfg.isMtvu = (std::getenv("DC2_G297_NO_MTVU") == nullptr);
        startupCfg.isMtgs = (std::getenv("DC2_G150_NO_MTGS") == nullptr);
        startupCfg.isProfiler = (std::getenv("DC2_PROFILE") != nullptr);
        startupCfg.dataPath = filePathStr.c_str();
        dc2_log::logStartupSummary(startupCfg);

        PS2Runtime runtime;
        if (!runtime.initialize(windowTitle.c_str()))
        {
            LOG_ERROR(Runtime, "Failed to initialize PS2 runtime");
            return 1;
        }

        registerAllFunctions(runtime);

        if (!runtime.loadELF(filePathStr))
        {
            LOG_ERROR(Runtime, "Failed to load ELF file: %s", filePathStr.c_str());
            return 1;
        }

        runtime.run();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        dc2_log::flush();
        dc2_log::shutdown();
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        LOG_FATAL(Runtime, "[main] fatal exception: %s", e.what());
    }
    catch (...)
    {
        LOG_FATAL(Runtime, "[main] fatal exception: unknown");
    }

    dc2_log::flush();
    dc2_log::shutdown();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
