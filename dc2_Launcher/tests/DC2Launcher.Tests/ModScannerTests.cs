using System;
using System.IO;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class ModScannerTests
{
    [Fact]
    public void ModOverrideResolver_ModsEnabled_FileInMods_ReturnsModPath()
    {
        var fs = new MockFileSystemService();
        var modsRoot = @"C:\Launcher\Mods";
        var dataRoot = @"C:\Launcher\DATA";

        fs.ExistingFiles.Add(@"C:\Launcher\Mods\MOVIE\intro.m2v");
        fs.ExistingFiles.Add(@"C:\Launcher\DATA\MOVIE\intro.m2v");

        var effective = ModOverrideResolver.ResolveEffectiveAssetPath(
            @"MOVIE\intro.m2v",
            modsRoot,
            dataRoot,
            modsEnabled: true,
            fs);

        Assert.Equal(@"C:\Launcher\Mods\MOVIE\intro.m2v", effective);
    }

    [Fact]
    public void ModOverrideResolver_ModsEnabled_FileMissingInMods_FallsBackToDataPath()
    {
        var fs = new MockFileSystemService();
        var modsRoot = @"C:\Launcher\Mods";
        var dataRoot = @"C:\Launcher\DATA";

        fs.ExistingFiles.Add(@"C:\Launcher\DATA\SOUND\bgm.sound");

        var effective = ModOverrideResolver.ResolveEffectiveAssetPath(
            @"SOUND\bgm.sound",
            modsRoot,
            dataRoot,
            modsEnabled: true,
            fs);

        Assert.Equal(@"C:\Launcher\DATA\SOUND\bgm.sound", effective);
    }

    [Fact]
    public void ModOverrideResolver_ModsDisabled_ReturnsDataPath()
    {
        var fs = new MockFileSystemService();
        var modsRoot = @"C:\Launcher\Mods";
        var dataRoot = @"C:\Launcher\DATA";

        fs.ExistingFiles.Add(@"C:\Launcher\Mods\MOVIE\intro.m2v");
        fs.ExistingFiles.Add(@"C:\Launcher\DATA\MOVIE\intro.m2v");

        var effective = ModOverrideResolver.ResolveEffectiveAssetPath(
            @"MOVIE\intro.m2v",
            modsRoot,
            dataRoot,
            modsEnabled: false,
            fs);

        Assert.Equal(@"C:\Launcher\DATA\MOVIE\intro.m2v", effective);
    }

    [Fact]
    public void ModScannerService_ScanModsDirectory_DiscoversModFiles()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        var modsDir = Path.Combine(tempDir, "Mods");
        var movieSubdir = Path.Combine(modsDir, "MOVIE");
        Directory.CreateDirectory(movieSubdir);

        var testModFile = Path.Combine(movieSubdir, "intro.m2v");
        File.WriteAllText(testModFile, "DUMMY MOD DATA CONTENT");

        try
        {
            var logger = new MockLoggerService();
            var scanner = new ModScannerService(logger);

            var result = scanner.ScanModsDirectory(modsDir);

            Assert.True(result.Success);
            Assert.Single(result.ModFiles);
            Assert.Equal(@"MOVIE\intro.m2v", result.ModFiles[0].RelativePath);
            Assert.True(result.ModFiles[0].FileSizeBytes > 0);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }

    [Fact]
    public void ModScannerService_SaveManifest_WritesValidJsonManifest()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir);

        try
        {
            var logger = new MockLoggerService();
            var scanner = new ModScannerService(logger);

            var scanResult = new ModScanResult();
            scanResult.ModFiles.Add(new ModFileInfo
            {
                RelativePath = @"TEXTURES\hero.png",
                FileSizeBytes = 2048,
                LastModifiedUtc = DateTime.UtcNow
            });

            var manifestPath = Path.Combine(tempDir, "Config", "mods_manifest.json");
            scanner.SaveManifest(manifestPath, scanResult);

            Assert.True(File.Exists(manifestPath));
            var json = File.ReadAllText(manifestPath);

            Assert.Contains("schemaVersion", json);
            Assert.Contains("TEXTURES", json);
            Assert.Contains("hero.png", json);
            Assert.Contains("2048", json);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }
}
