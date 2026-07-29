using System;
using System.Collections.Generic;

namespace DC2Launcher.Core.Models;

public class ModFileInfo
{
    public string RelativePath { get; set; } = string.Empty;
    public long FileSizeBytes { get; set; }
    public DateTime LastModifiedUtc { get; set; }
}

public class ModScanResult
{
    public bool Success { get; set; } = true;
    public List<ModFileInfo> ModFiles { get; set; } = new();
    public List<string> Warnings { get; set; } = new();
    public List<string> Errors { get; set; } = new();
}

public class ModsManifest
{
    public int SchemaVersion { get; set; } = 1;
    public DateTime GeneratedAtUtc { get; set; } = DateTime.UtcNow;
    public int TotalModFiles { get; set; }
    public List<ModFileInfo> Files { get; set; } = new();
}
