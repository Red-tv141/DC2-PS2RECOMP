using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class HardeningTests
{
    [Fact]
    public void ValidationService_MissingRunnerExecutable_ProducesError()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var settings = new LauncherSettings();

        var readiness = validator.ValidateLaunchReadiness(settings);

        Assert.False(readiness.IsValid);
        Assert.Contains(readiness.Errors, e => e.Contains("Runner executable missing", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void ControllerConfigService_CorruptJson_RecoversWithDefaults()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir);

        try
        {
            var logger = new MockLoggerService();
            var service = new ControllerConfigService(logger);

            var configPath = Path.Combine(tempDir, "Config", "controller.json");
            Directory.CreateDirectory(Path.GetDirectoryName(configPath)!);
            File.WriteAllText(configPath, "{ CORRUPT INVALID JSON CONTENT !!! }");

            var loaded = service.LoadConfig(configPath);

            Assert.NotNull(loaded);
            Assert.Equal(1, loaded.SchemaVersion);
            Assert.Equal(10.0, loaded.LeftStickDeadZonePercent);
            Assert.Equal(24, loaded.Mappings.Count);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }

    [Fact]
    public void PathResolver_HandlesSpacesAndNonAsciiCharacters()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\Games\Dark Cloud 2 (v2.00) — 🎮" };
        var resolver = new PathResolver(env);

        var relativePath = @"DATA\SubFolder (Special) — Test\file.txt";
        var resolved = resolver.Resolve(relativePath);

        var expected = Path.Combine(@"C:\Games\Dark Cloud 2 (v2.00) — 🎮", relativePath);
        Assert.Equal(expected, resolved);
    }

    [Fact]
    public async Task IsoExtractor_Cancellation_CleansUpTempFolder()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir);

        try
        {
            var env = new TestLauncherEnvironment { LauncherRoot = tempDir };
            var fs = new MockFileSystemService();
            var logger = new MockLoggerService();
            var extractor = new DiscUtilsIsoExtractor(env, fs, logger);

            var cts = new CancellationTokenSource();
            cts.Cancel(); // Pre-cancelled token

            var isoPath = Path.Combine(tempDir, "dummy.iso");
            fs.ExistingFiles.Add(isoPath);

            var success = await extractor.ExtractIsoAsync(isoPath, "DATA", null, cts.Token);

            Assert.False(success);
            var tempExtractingDir = Path.Combine(tempDir, "DATA.extracting");
            Assert.False(Directory.Exists(tempExtractingDir), "DATA.extracting temp folder must be cleaned up on cancellation");
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }
}
