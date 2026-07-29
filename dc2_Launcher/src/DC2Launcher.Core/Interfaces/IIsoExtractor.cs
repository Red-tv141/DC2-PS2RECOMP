using System;
using System.Threading;
using System.Threading.Tasks;
using DC2Launcher.Core.Models;

namespace DC2Launcher.Core.Interfaces;

public interface IIsoExtractor
{
    bool IsoExists(string isoPath);
    bool IsDataPopulated(string dataPath);
    ExtractionState? GetExtractionState();
    Task<bool> ExtractIsoAsync(
        string isoPath, 
        string dataPath, 
        Action<double, string>? progressCallback, 
        CancellationToken cancellationToken);
}
