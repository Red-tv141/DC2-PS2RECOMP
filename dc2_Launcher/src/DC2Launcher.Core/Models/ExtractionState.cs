using System;

namespace DC2Launcher.Core.Models;

public class ExtractionState
{
    public int SchemaVersion { get; set; } = 1;
    public string IsoFileName { get; set; } = string.Empty;
    public long IsoLengthBytes { get; set; }
    public DateTime ExtractedTimestampUtc { get; set; }
    public string DetectedElfPath { get; set; } = string.Empty;
    public bool Success { get; set; }
}
