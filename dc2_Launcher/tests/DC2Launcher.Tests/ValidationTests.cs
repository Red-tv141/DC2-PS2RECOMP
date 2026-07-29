using System;
using System.Collections.Generic;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class MockFileSystemService : Core.Interfaces.IFileSystemService
{
    public HashSet<string> ExistingFiles { get; } = new(StringComparer.OrdinalIgnoreCase);
    public HashSet<string> ExistingDirectories { get; } = new(StringComparer.OrdinalIgnoreCase);
    public List<string> OpenedFolders { get; } = new();

    public bool FileExists(string path) => ExistingFiles.Contains(path);
    public bool DirectoryExists(string path) => ExistingDirectories.Contains(path);
    public long GetFileSize(string path) => FileExists(path) ? 1024 : 0;
    public void CreateDirectory(string path) => ExistingDirectories.Add(path);
    public void OpenFolderInExplorer(string path) => OpenedFolders.Add(path);
}

public class ValidationTests
{
    [Fact]
    public void ValidateSettings_ValidSettings_ReturnsSuccess()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var settings = new LauncherSettings();

        var result = validator.ValidateSettings(settings);

        Assert.True(result.IsValid);
        Assert.Empty(result.Errors);
    }

    [Fact]
    public void ValidateSettings_CustomResolutionInvalidWidthOrHeight_ReturnsErrors()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var settings = new LauncherSettings();
        settings.Game.Resolution.Mode = "Custom";
        settings.Game.Resolution.Width = -100;
        settings.Game.Resolution.Height = 0;

        var result = validator.ValidateSettings(settings);

        Assert.False(result.IsValid);
        Assert.Equal(2, result.Errors.Count);
    }

    [Fact]
    public void ValidateSettings_DuplicateMemoryCardPaths_ReturnsWarningError()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var settings = new LauncherSettings();
        settings.Paths.MemoryCard1 = @"Saves\SameFolder";
        settings.Paths.MemoryCard2 = @"Saves\SameFolder";

        var result = validator.ValidateSettings(settings);

        Assert.False(result.IsValid);
        Assert.Contains(result.Errors, e => e.Contains("exact same directory"));
    }

    [Fact]
    public void ValidateLaunchReadiness_MissingAllFiles_ReturnsConsolidatedErrorList()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService(); // No files exist
        var validator = new ValidationService(env, fs);
        var settings = new LauncherSettings();

        var result = validator.ValidateLaunchReadiness(settings);

        Assert.False(result.IsValid);
        Assert.Contains(result.Errors, e => e.Contains("Runner executable missing"));
        Assert.Contains(result.Errors, e => e.Contains("Disc ELF file missing"));
        Assert.Contains(result.Errors, e => e.Contains("DATA directory missing"));
        Assert.Contains(result.Errors, e => e.Contains("Game ISO missing"));
    }

    [Fact]
    public void ValidateLaunchReadiness_AllFilesPresent_ReturnsSuccess()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        fs.ExistingFiles.Add(@"C:\MockLauncher\bin\dc2_runner.exe");
        fs.ExistingFiles.Add(@"C:\MockLauncher\DATA\SCUS_972.13");
        fs.ExistingFiles.Add(@"C:\MockLauncher\Dark Cloud 2 (USA) (v2.00).iso");
        fs.ExistingDirectories.Add(@"C:\MockLauncher\DATA");

        var validator = new ValidationService(env, fs);
        var settings = new LauncherSettings();

        var result = validator.ValidateLaunchReadiness(settings);

        Assert.True(result.IsValid);
    }
}
