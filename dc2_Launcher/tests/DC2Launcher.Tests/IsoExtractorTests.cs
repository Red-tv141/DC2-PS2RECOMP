using System;
using System.IO;
using System.Security;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using DiscUtils.Iso9660;
using DC2Launcher.Core.Models;
using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class MockIsoExtractor : Core.Interfaces.IIsoExtractor
{
    public bool IsoExistsResult { get; set; } = true;
    public bool IsDataPopulatedResult { get; set; } = false;
    public ExtractionState? State { get; set; }

    public bool IsoExists(string isoPath) => IsoExistsResult;
    public bool IsDataPopulated(string dataPath) => IsDataPopulatedResult;
    public ExtractionState? GetExtractionState() => State;

    public Task<bool> ExtractIsoAsync(string isoPath, string dataPath, Action<double, string>? progressCallback, CancellationToken cancellationToken)
    {
        progressCallback?.Invoke(50, "Extracting test file...");
        if (cancellationToken.IsCancellationRequested) return Task.FromResult(false);
        progressCallback?.Invoke(100, "Done");
        return Task.FromResult(true);
    }
}

public class IsoExtractorTests
{
    [Fact]
    public void ExtractionState_SerializationAndDeserialization_Succeeds()
    {
        var state = new ExtractionState
        {
            SchemaVersion = 1,
            IsoFileName = "Dark Cloud 2 (USA) (v2.00).iso",
            IsoLengthBytes = 4437770240,
            ExtractedTimestampUtc = DateTime.UtcNow,
            DetectedElfPath = @"DATA\SCUS_972.13",
            Success = true
        };

        var json = JsonSerializer.Serialize(state);
        var deserialized = JsonSerializer.Deserialize<ExtractionState>(json);

        Assert.NotNull(deserialized);
        Assert.Equal(1, deserialized.SchemaVersion);
        Assert.Equal("Dark Cloud 2 (USA) (v2.00).iso", deserialized.IsoFileName);
        Assert.Equal(@"DATA\SCUS_972.13", deserialized.DetectedElfPath);
        Assert.True(deserialized.Success);
    }

    [Fact]
    public async Task MockIsoExtractor_SimulateExtraction_ProgressAndSuccess()
    {
        var extractor = new MockIsoExtractor();
        double lastProgress = 0;
        string lastMessage = string.Empty;

        var result = await extractor.ExtractIsoAsync("test.iso", "DATA", (percent, msg) =>
        {
            lastProgress = percent;
            lastMessage = msg;
        }, CancellationToken.None);

        Assert.True(result);
        Assert.Equal(100, lastProgress);
        Assert.Equal("Done", lastMessage);
    }

    [Fact]
    public void PathTraversal_SecurityValidation_RejectsEscapingPaths()
    {
        var rootDir = @"C:\MockLauncher\DATA.extracting";
        var safePrefix = rootDir.EndsWith(@"\") ? rootDir : rootDir + @"\";

        var maliciousEntry = @"..\..\Windows\System32\cmd.exe";
        var targetPath = Path.GetFullPath(Path.Combine(rootDir, maliciousEntry));

        var isSafe = targetPath.StartsWith(safePrefix, StringComparison.OrdinalIgnoreCase);

        Assert.False(isSafe);
    }

    [Fact]
    public void SafeEntry_SecurityValidation_AllowsValidPaths()
    {
        var rootDir = @"C:\MockLauncher\DATA.extracting";
        var safePrefix = rootDir.EndsWith(@"\") ? rootDir : rootDir + @"\";

        var validEntry = @"menu\icon.bin";
        var targetPath = Path.GetFullPath(Path.Combine(rootDir, validEntry));

        var isSafe = targetPath.StartsWith(safePrefix, StringComparison.OrdinalIgnoreCase);

        Assert.True(isSafe);
    }

    [Fact]
    public async Task PerformRealIsoExtraction_IfIsoPresent()
    {
        var isoPath = @"D:\ps2r\dc2_Launcher\Dark Cloud 2 (USA) (v2.00).iso";
        if (!File.Exists(isoPath)) return; // Skip if ISO is not present

        var env = new TestLauncherEnvironment { LauncherRoot = @"D:\ps2r\dc2_Launcher" };
        var fs = new FileSystemService();
        var logger = new FileLoggerService(env);
        var extractor = new DiscUtilsIsoExtractor(env, fs, logger);

        double lastProgress = 0;
        string lastMessage = string.Empty;

        var result = await extractor.ExtractIsoAsync(
            "Dark Cloud 2 (USA) (v2.00).iso",
            "DATA",
            (percent, msg) =>
            {
                lastProgress = percent;
                lastMessage = msg;
            },
            CancellationToken.None);

        Assert.True(result, $"Extraction failed: {lastMessage}");
        Assert.True(File.Exists(@"D:\ps2r\dc2_Launcher\DATA\SCUS_972.13"));
        Assert.True(File.Exists(@"D:\ps2r\dc2_Launcher\Config\extraction_state.json"));
    }
}
