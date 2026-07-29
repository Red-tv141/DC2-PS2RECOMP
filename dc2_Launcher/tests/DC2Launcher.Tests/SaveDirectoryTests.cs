using System;
using System.IO;
using DC2Launcher.App.ViewModels;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class SaveDirectoryTests
{
    [Fact]
    public void ValidationService_SameMemoryCardPaths_ProducesWarningError()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);

        var settings = new LauncherSettings();
        settings.Paths.MemoryCard1 = @"Saves\SharedFolder";
        settings.Paths.MemoryCard2 = @"Saves\SharedFolder";

        var result = validator.ValidateSettings(settings);

        Assert.False(result.IsValid);
        Assert.Contains(result.Errors, e => e.Contains("exact same directory"));
    }

    [Fact]
    public void ValidationService_DistinctMemoryCardPaths_ReturnsNoSameFolderWarning()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);

        var settings = new LauncherSettings();
        settings.Paths.MemoryCard1 = @"Saves\MemoryCard1";
        settings.Paths.MemoryCard2 = @"Saves\MemoryCard2";

        var result = validator.ValidateSettings(settings);

        Assert.True(result.IsValid);
        Assert.DoesNotContain(result.Errors, e => e.Contains("exact same directory"));
    }

    [Fact]
    public void ProcessLauncher_BuildLaunchConfiguration_EmitsMemCardDirEnvironmentVariables()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var logger = new MockLoggerService();
        var launcher = new ProcessLauncher(env, validator, logger);

        var settings = new LauncherSettings();
        settings.Paths.MemoryCard1 = @"Saves\Card1";
        settings.Paths.MemoryCard2 = @"E:\ExternalSaves\Card2";

        var config = launcher.BuildLaunchConfiguration(settings);

        Assert.True(config.EnvironmentVariables.ContainsKey("DC2_MEMCARD1_DIR"));
        Assert.Equal(Path.GetFullPath(@"C:\MockLauncher\Saves\Card1"), config.EnvironmentVariables["DC2_MEMCARD1_DIR"]);

        Assert.True(config.EnvironmentVariables.ContainsKey("DC2_MEMCARD2_DIR"));
        Assert.Equal(@"E:\ExternalSaves\Card2", config.EnvironmentVariables["DC2_MEMCARD2_DIR"]);
    }

    [Fact]
    public void MainViewModel_ResetMemCardCommands_RestoreDefaultPaths()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir);

        try
        {
            var env = new TestLauncherEnvironment { LauncherRoot = tempDir };
            var fs = new MockFileSystemService();
            var logger = new MockLoggerService();
            var settingsService = new JsonSettingsService(env, logger);
            var validationService = new ValidationService(env, fs);
            var processLauncher = new ProcessLauncher(env, validationService, logger);
            var isoExtractor = new MockIsoExtractor();

            var vm = new MainViewModel(env, settingsService, fs, processLauncher, validationService, isoExtractor);

            vm.MemoryCard1Path = @"Custom\Path1";
            vm.MemoryCard2Path = @"Custom\Path2";

            Assert.Equal(@"Custom\Path1", vm.MemoryCard1Path);
            Assert.Equal(@"Custom\Path2", vm.MemoryCard2Path);

            vm.ResetMemCard1Command.Execute(null);
            Assert.Equal(@"Saves\MemoryCard1", vm.MemoryCard1Path);

            vm.ResetMemCard2Command.Execute(null);
            Assert.Equal(@"Saves\MemoryCard2", vm.MemoryCard2Path);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }

    [Fact]
    public void MainViewModel_OpenMemCardFolder_CreatesDirectorySafely()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir);

        try
        {
            var env = new TestLauncherEnvironment { LauncherRoot = tempDir };
            var fs = new MockFileSystemService();
            var logger = new MockLoggerService();
            var settingsService = new JsonSettingsService(env, logger);
            var validationService = new ValidationService(env, fs);
            var processLauncher = new ProcessLauncher(env, validationService, logger);
            var isoExtractor = new MockIsoExtractor();

            var vm = new MainViewModel(env, settingsService, fs, processLauncher, validationService, isoExtractor);

            vm.OpenMemCard1FolderCommand.Execute(null);

            var expectedPath = Path.Combine(tempDir, @"Saves\MemoryCard1");
            Assert.True(fs.DirectoryExists(expectedPath));
            Assert.Contains(expectedPath, fs.OpenedFolders);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }
}
