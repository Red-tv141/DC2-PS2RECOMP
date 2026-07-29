using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;

namespace DC2Launcher.Infrastructure.Services;

public class StubIsoExtractor : IIsoExtractor
{
    public bool IsoExists(string isoPath) => File.Exists(isoPath);
    public bool IsDataPopulated(string dataPath) => Directory.Exists(dataPath);
    public ExtractionState? GetExtractionState() => null;

    public Task<bool> ExtractIsoAsync(string isoPath, string destinationDirectory, Action<double, string>? progressCallback, CancellationToken cancellationToken)
    {
        progressCallback?.Invoke(0, "ISO extraction pipeline active.");
        return Task.FromResult(false);
    }
}
