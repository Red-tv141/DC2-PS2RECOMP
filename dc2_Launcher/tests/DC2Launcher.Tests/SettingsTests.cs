using System;
using System.IO;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class MockLoggerService : ILoggerService
{
    public string LogFilePath => "mock.log";
    public void LogInfo(string message) { }
    public void LogWarning(string message) { }
    public void LogError(string message, Exception? ex = null) { }
}

public class SettingsTests
{
    [Fact]
    public void DefaultSettings_HaveCorrectDefaults()
    {
        var settings = new LauncherSettings();

        Assert.Equal(1, settings.SchemaVersion);
        Assert.False(settings.Game.SkipIntro);
        Assert.False(settings.Game.Enable60Fps);
        Assert.False(settings.Game.EnableDebugMenu);
        Assert.Equal("GameDefault", settings.Game.Resolution.Mode);
        Assert.Equal(@"bin\dc2_runner.exe", settings.Paths.Runner);
        Assert.Equal(@"DATA\SCUS_972.13", settings.Paths.Elf);
    }

    [Fact]
    public void JsonSettingsService_SavesAndLoadsSettings()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir);

        try
        {
            var env = new TestLauncherEnvironment { LauncherRoot = tempDir };
            var logger = new MockLoggerService();
            var service = new JsonSettingsService(env, logger);

            var settings = service.LoadSettings();
            settings.Game.Enable60Fps = true;
            settings.Game.Resolution.Mode = "1920x1080";
            service.SaveSettings(settings);

            var reloaded = service.LoadSettings();
            Assert.True(reloaded.Game.Enable60Fps);
            Assert.Equal("1920x1080", reloaded.Game.Resolution.Mode);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }

    [Fact]
    public void JsonSettingsService_PreservesExternalAbsoluteSavePaths()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir);

        try
        {
            var env = new TestLauncherEnvironment { LauncherRoot = tempDir };
            var logger = new MockLoggerService();
            var service = new JsonSettingsService(env, logger);

            var settings = service.LoadSettings();
            var externalPath = @"E:\ExternalSaves\Card1";
            settings.Paths.MemoryCard1 = externalPath;
            service.SaveSettings(settings);

            var reloaded = service.LoadSettings();
            Assert.Equal(externalPath, reloaded.Paths.MemoryCard1);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }

    [Fact]
    public void JsonSettingsService_LauncherRelocation_ResolvesRelativePathsCorrectly()
    {
        var tempDir1 = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var tempDir2 = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir1);
        Directory.CreateDirectory(tempDir2);

        try
        {
            var env1 = new TestLauncherEnvironment { LauncherRoot = tempDir1 };
            var logger = new MockLoggerService();
            var service1 = new JsonSettingsService(env1, logger);

            var settings = service1.LoadSettings();
            settings.Paths.MemoryCard1 = @"Saves\MemoryCard1"; // Relative
            service1.SaveSettings(settings);

            // Copy settings file to relocated launcher folder (simulating moving launcher directory)
            var config1 = Path.Combine(tempDir1, "Config", "launcher_settings.json");
            var config2Dir = Path.Combine(tempDir2, "Config");
            Directory.CreateDirectory(config2Dir);
            var config2 = Path.Combine(config2Dir, "launcher_settings.json");
            File.Copy(config1, config2);

            var env2 = new TestLauncherEnvironment { LauncherRoot = tempDir2 };
            var service2 = new JsonSettingsService(env2, logger);
            var reloaded = service2.LoadSettings();

            var resolver2 = new PathResolver(env2);
            var resolvedMemCard = resolver2.Resolve(reloaded.Paths.MemoryCard1);

            Assert.Equal(Path.GetFullPath(Path.Combine(tempDir2, @"Saves\MemoryCard1")), resolvedMemCard);
        }
        finally
        {
            if (Directory.Exists(tempDir1)) Directory.Delete(tempDir1, true);
            if (Directory.Exists(tempDir2)) Directory.Delete(tempDir2, true);
        }
    }

    [Fact]
    public void JsonSettingsService_CorruptJson_RecoversWithDefaultsAndBackup()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var configDir = Path.Combine(tempDir, "Config");
        Directory.CreateDirectory(configDir);

        var settingsPath = Path.Combine(configDir, "launcher_settings.json");
        File.WriteAllText(settingsPath, "INVALID JSON CONTENT {{{");

        try
        {
            var env = new TestLauncherEnvironment { LauncherRoot = tempDir };
            var logger = new MockLoggerService();
            var service = new JsonSettingsService(env, logger);

            var loaded = service.LoadSettings();
            Assert.NotNull(loaded);
            Assert.Equal(1, loaded.SchemaVersion);

            var backupFiles = Directory.GetFiles(configDir, "*.bak");
            Assert.Single(backupFiles);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }
}
