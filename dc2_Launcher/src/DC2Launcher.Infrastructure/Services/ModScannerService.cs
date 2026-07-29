using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;

namespace DC2Launcher.Infrastructure.Services;

public class ModScannerService : IModScannerService
{
    private readonly ILoggerService _logger;
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    public ModScannerService(ILoggerService logger)
    {
        _logger = logger ?? throw new ArgumentNullException(nameof(logger));
    }

    public ModScanResult ScanModsDirectory(string modsDirectoryPath)
    {
        var result = new ModScanResult();

        if (string.IsNullOrWhiteSpace(modsDirectoryPath) || !Directory.Exists(modsDirectoryPath))
        {
            _logger.LogInfo($"Mods directory '{modsDirectoryPath}' does not exist or is empty. Scan completed cleanly.");
            return result;
        }

        var fullModsRoot = Path.GetFullPath(modsDirectoryPath);
        var safeRootPrefix = fullModsRoot.EndsWith(Path.DirectorySeparatorChar.ToString(), StringComparison.Ordinal)
            ? fullModsRoot
            : fullModsRoot + Path.DirectorySeparatorChar;

        var seenRelativePathsLower = new Dictionary<string, string>(StringComparer.Ordinal);

        try
        {
            var files = Directory.GetFiles(fullModsRoot, "*", SearchOption.AllDirectories);

            foreach (var filePath in files)
            {
                var fullFilePath = Path.GetFullPath(filePath);

                // Security Check 1: Path Traversal
                if (!fullFilePath.StartsWith(safeRootPrefix, StringComparison.OrdinalIgnoreCase))
                {
                    var error = $"Security Warning: File '{filePath}' escapes root mods directory '{fullModsRoot}'. Skipped.";
                    result.Warnings.Add(error);
                    _logger.LogWarning(error);
                    continue;
                }

                // Security Check 2: Reparse Point / Symlink Escaping Check
                try
                {
                    var fileInfo = new FileInfo(fullFilePath);
                    if (fileInfo.Attributes.HasFlag(FileAttributes.ReparsePoint))
                    {
                        var warning = $"Reparse Point Warning: File/Link '{filePath}' is a symlink or junction. Verify link target safety.";
                        result.Warnings.Add(warning);
                    }
                }
                catch (Exception ex)
                {
                    var warning = $"Unreadable File Attributes: Cannot check file attributes for '{filePath}': {ex.Message}";
                    result.Warnings.Add(warning);
                    continue;
                }

                // Relative Path Calculation
                var relativePath = fullFilePath.Substring(safeRootPrefix.Length);
                var lowerRelative = relativePath.ToLowerInvariant();

                // Security Check 3: Case-Duplicate Collision Detection
                if (seenRelativePathsLower.TryGetValue(lowerRelative, out var existingPath))
                {
                    if (!string.Equals(existingPath, relativePath, StringComparison.Ordinal))
                    {
                        var warning = $"Case Collision Warning: Mod file '{relativePath}' conflicts with existing file '{existingPath}'. Windows filesystems are case-insensitive.";
                        result.Warnings.Add(warning);
                        _logger.LogWarning(warning);
                    }
                }
                else
                {
                    seenRelativePathsLower[lowerRelative] = relativePath;
                }

                // Security Check 4: Unreadable File Verification
                try
                {
                    var fi = new FileInfo(fullFilePath);
                    result.ModFiles.Add(new ModFileInfo
                    {
                        RelativePath = relativePath,
                        FileSizeBytes = fi.Length,
                        LastModifiedUtc = fi.LastWriteTimeUtc
                    });
                }
                catch (Exception ex)
                {
                    var warning = $"Unreadable File Error: Failed to read file metadata for '{filePath}': {ex.Message}";
                    result.Warnings.Add(warning);
                    _logger.LogWarning(warning);
                }
            }

            _logger.LogInfo($"Mod scan completed. Discovered {result.ModFiles.Count} mod file(s) with {result.Warnings.Count} warning(s).");
        }
        catch (Exception ex)
        {
            result.Success = false;
            var error = $"Error during mod directory scan: {ex.Message}";
            result.Errors.Add(error);
            _logger.LogError(error, ex);
        }

        return result;
    }

    public void SaveManifest(string manifestFilePath, ModScanResult scanResult)
    {
        if (string.IsNullOrWhiteSpace(manifestFilePath)) throw new ArgumentNullException(nameof(manifestFilePath));
        if (scanResult == null) throw new ArgumentNullException(nameof(scanResult));

        try
        {
            var manifestDir = Path.GetDirectoryName(manifestFilePath);
            if (!string.IsNullOrWhiteSpace(manifestDir) && !Directory.Exists(manifestDir))
            {
                Directory.CreateDirectory(manifestDir);
            }

            var manifest = new ModsManifest
            {
                SchemaVersion = 1,
                GeneratedAtUtc = DateTime.UtcNow,
                TotalModFiles = scanResult.ModFiles.Count,
                Files = scanResult.ModFiles
            };

            var json = JsonSerializer.Serialize(manifest, JsonOptions);
            var tempFilePath = manifestFilePath + ".tmp";

            File.WriteAllText(tempFilePath, json);

            if (File.Exists(manifestFilePath))
            {
                File.Replace(tempFilePath, manifestFilePath, null);
            }
            else
            {
                File.Move(tempFilePath, manifestFilePath);
            }

            _logger.LogInfo($"Mods manifest saved successfully to '{manifestFilePath}'.");
        }
        catch (Exception ex)
        {
            _logger.LogError($"Failed to save mods manifest to '{manifestFilePath}'", ex);
        }
    }
}
