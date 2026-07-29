using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using DiscUtils.Iso9660;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;

namespace DC2Launcher.Infrastructure.Services;

public class DiscUtilsIsoExtractor : IIsoExtractor
{
    private readonly ILauncherEnvironment _environment;
    private readonly IFileSystemService _fileSystem;
    private readonly ILoggerService _logger;
    private readonly PathResolver _pathResolver;
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    public DiscUtilsIsoExtractor(
        ILauncherEnvironment environment,
        IFileSystemService fileSystem,
        ILoggerService logger)
    {
        _environment = environment ?? throw new ArgumentNullException(nameof(environment));
        _fileSystem = fileSystem ?? throw new ArgumentNullException(nameof(fileSystem));
        _logger = logger ?? throw new ArgumentNullException(nameof(logger));
        _pathResolver = new PathResolver(_environment);
    }

    public bool IsoExists(string isoPath)
    {
        var resolved = _pathResolver.Resolve(isoPath);
        return _fileSystem.FileExists(resolved);
    }

    public bool IsDataPopulated(string dataPath)
    {
        var resolvedData = _pathResolver.Resolve(dataPath);
        if (!_fileSystem.DirectoryExists(resolvedData)) return false;

        var elfPath = Path.Combine(resolvedData, "SCUS_972.13");
        return _fileSystem.FileExists(elfPath);
    }

    public ExtractionState? GetExtractionState()
    {
        var statePath = Path.Combine(_environment.LauncherRoot, "Config", "extraction_state.json");
        if (!_fileSystem.FileExists(statePath)) return null;

        try
        {
            var json = File.ReadAllText(statePath);
            return JsonSerializer.Deserialize<ExtractionState>(json, JsonOptions);
        }
        catch
        {
            return null;
        }
    }

    public async Task<bool> ExtractIsoAsync(
        string isoPath, 
        string dataPath, 
        Action<double, string>? progressCallback, 
        CancellationToken cancellationToken)
    {
        var resolvedIso = _pathResolver.Resolve(isoPath);
        var resolvedData = _pathResolver.Resolve(dataPath);
        var tempExtractingDir = Path.Combine(_environment.LauncherRoot, "DATA.extracting");

        if (!_fileSystem.FileExists(resolvedIso))
        {
            _logger.LogError($"Extraction failed: ISO not found at {resolvedIso}");
            progressCallback?.Invoke(0, $"Error: ISO missing at {resolvedIso}");
            return false;
        }

        try
        {
            // Clean up any stale incomplete extraction folder
            if (Directory.Exists(tempExtractingDir))
            {
                _logger.LogInfo("Cleaning previous temp extraction directory DATA.extracting...");
                Directory.Delete(tempExtractingDir, true);
            }

            Directory.CreateDirectory(tempExtractingDir);
            var safeRootPrefix = tempExtractingDir.EndsWith(Path.DirectorySeparatorChar.ToString()) 
                ? tempExtractingDir 
                : tempExtractingDir + Path.DirectorySeparatorChar;

            _logger.LogInfo($"Starting ISO extraction from '{resolvedIso}' to '{tempExtractingDir}'");
            progressCallback?.Invoke(0, "Opening ISO image...");

            await Task.Run(() =>
            {
                using var isoStream = File.OpenRead(resolvedIso);
                using var cdReader = new CDReader(isoStream, false);

                var fileList = GetAllIsoFiles(cdReader, @"\");
                long totalBytes = 0;

                foreach (var fileEntry in fileList)
                {
                    totalBytes += cdReader.GetFileLength(fileEntry);
                }

                _logger.LogInfo($"ISO contains {fileList.Count} files ({totalBytes} total bytes).");

                long extractedBytes = 0;
                var buffer = new byte[81920]; // 80KB buffer

                for (int i = 0; i < fileList.Count; i++)
                {
                    cancellationToken.ThrowIfCancellationRequested();

                    var discPath = fileList[i];
                    var relativePath = discPath.TrimStart('\\', '/');

                    // Strip ISO9660 version suffix (e.g. SCUS_972.13;1 -> SCUS_972.13, DMMY.;1 -> DMMY.)
                    int semicolonIdx = relativePath.IndexOf(';');
                    if (semicolonIdx >= 0)
                    {
                        relativePath = relativePath.Substring(0, semicolonIdx);
                    }
                    relativePath = relativePath.TrimEnd('.');

                    // Sanitize target destination path against path traversal
                    var targetFilePath = Path.GetFullPath(Path.Combine(tempExtractingDir, relativePath));
                    if (!targetFilePath.StartsWith(safeRootPrefix, StringComparison.OrdinalIgnoreCase))
                    {
                        throw new SecurityException($"Path traversal rejected for ISO entry: '{discPath}' -> '{targetFilePath}'");
                    }

                    var targetDir = Path.GetDirectoryName(targetFilePath);
                    if (!string.IsNullOrEmpty(targetDir) && !Directory.Exists(targetDir))
                    {
                        Directory.CreateDirectory(targetDir);
                    }

                    using var sourceStream = cdReader.OpenFile(discPath, FileMode.Open);
                    using var destStream = File.Create(targetFilePath);

                    int bytesRead;
                    while ((bytesRead = sourceStream.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        destStream.Write(buffer, 0, bytesRead);
                        extractedBytes += bytesRead;

                        if (totalBytes > 0)
                        {
                            double progressPercent = (double)extractedBytes / totalBytes * 100.0;
                            progressCallback?.Invoke(progressPercent, $"Extracting {Path.GetFileName(targetFilePath)} ({i + 1}/{fileList.Count})");
                        }
                    }
                }
            }, cancellationToken);

            // Verify SCUS_972.13 post-extraction in DATA.extracting
            var extractedElfPath = Path.Combine(tempExtractingDir, "SCUS_972.13");
            if (!File.Exists(extractedElfPath))
            {
                // Search recursively for SCUS_972.13 if located in a subfolder or version-suffixed
                var matches = Directory.GetFiles(tempExtractingDir, "SCUS_972.13*", SearchOption.AllDirectories);
                if (matches.Length > 0)
                {
                    extractedElfPath = matches[0];
                }
                else
                {
                    throw new FileNotFoundException("Post-extraction verification failed: SCUS_972.13 missing from extracted files.");
                }
            }

            _logger.LogInfo($"Verified disc ELF at: {extractedElfPath}");

            // Atomic directory replacement: Move tempExtractingDir -> DATA
            if (Directory.Exists(resolvedData))
            {
                _logger.LogInfo($"Replacing existing DATA folder at '{resolvedData}'...");
                Directory.Delete(resolvedData, true);
            }

            Directory.Move(tempExtractingDir, resolvedData);
            _logger.LogInfo($"DATA directory ready at '{resolvedData}'");

            var finalElfPath = Path.Combine(resolvedData, "SCUS_972.13");
            var relativeElfPath = _pathResolver.MakeRelativeIfUnderRoot(finalElfPath);

            // Record extraction_state.json
            var isoFileInfo = new FileInfo(resolvedIso);
            var state = new ExtractionState
            {
                SchemaVersion = 1,
                IsoFileName = Path.GetFileName(resolvedIso),
                IsoLengthBytes = isoFileInfo.Length,
                ExtractedTimestampUtc = DateTime.UtcNow,
                DetectedElfPath = relativeElfPath,
                Success = true
            };

            SaveExtractionState(state);

            progressCallback?.Invoke(100.0, "ISO Extraction completed successfully!");
            return true;
        }
        catch (OperationCanceledException)
        {
            _logger.LogWarning("ISO Extraction cancelled by user. Cleaning temp directory...");
            CleanTempDirectory(tempExtractingDir);
            progressCallback?.Invoke(0, "Extraction cancelled.");
            return false;
        }
        catch (Exception ex)
        {
            _logger.LogError("ISO Extraction failed.", ex);
            CleanTempDirectory(tempExtractingDir);
            progressCallback?.Invoke(0, $"Extraction Failed: {ex.Message}");
            return false;
        }
    }

    private List<string> GetAllIsoFiles(CDReader reader, string path)
    {
        var list = new List<string>();
        list.AddRange(reader.GetFiles(path));
        foreach (var dir in reader.GetDirectories(path))
        {
            list.AddRange(GetAllIsoFiles(reader, dir));
        }
        return list;
    }

    private void CleanTempDirectory(string tempDir)
    {
        try
        {
            if (Directory.Exists(tempDir))
            {
                Directory.Delete(tempDir, true);
            }
        }
        catch (Exception ex)
        {
            _logger.LogWarning($"Failed to clean temp extraction directory '{tempDir}': {ex.Message}");
        }
    }

    private void SaveExtractionState(ExtractionState state)
    {
        try
        {
            var configDir = Path.Combine(_environment.LauncherRoot, "Config");
            Directory.CreateDirectory(configDir);

            var statePath = Path.Combine(configDir, "extraction_state.json");
            var tempPath = statePath + ".tmp";
            var json = JsonSerializer.Serialize(state, JsonOptions);

            File.WriteAllText(tempPath, json);

            if (File.Exists(statePath))
            {
                File.Replace(tempPath, statePath, null);
            }
            else
            {
                File.Move(tempPath, statePath);
            }

            _logger.LogInfo("Config/extraction_state.json recorded successfully.");
        }
        catch (Exception ex)
        {
            _logger.LogError("Failed to save extraction_state.json", ex);
        }
    }
}
