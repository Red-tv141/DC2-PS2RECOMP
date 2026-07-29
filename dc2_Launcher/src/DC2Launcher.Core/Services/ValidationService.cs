using System;
using System.IO;
using System.Linq;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;

namespace DC2Launcher.Core.Services;

public class ValidationService : IValidationService
{
    private readonly ILauncherEnvironment _environment;
    private readonly IFileSystemService _fileSystemService;
    private readonly PathResolver _pathResolver;

    public ValidationService(ILauncherEnvironment environment, IFileSystemService fileSystemService)
    {
        _environment = environment ?? throw new ArgumentNullException(nameof(environment));
        _fileSystemService = fileSystemService ?? throw new ArgumentNullException(nameof(fileSystemService));
        _pathResolver = new PathResolver(_environment);
    }

    public ValidationResult ValidateSettings(LauncherSettings settings)
    {
        var result = new ValidationResult();
        if (settings == null)
        {
            result.AddError("Settings object is null.");
            return result;
        }

        // Custom Resolution Validation
        if (string.Equals(settings.Game.Resolution.Mode, "Custom", StringComparison.OrdinalIgnoreCase))
        {
            if (!settings.Game.Resolution.Width.HasValue || settings.Game.Resolution.Width.Value <= 0)
            {
                result.AddError("Custom resolution width must be a positive non-zero integer.");
            }
            if (!settings.Game.Resolution.Height.HasValue || settings.Game.Resolution.Height.Value <= 0)
            {
                result.AddError("Custom resolution height must be a positive non-zero integer.");
            }
        }

        // Memory Card Path Collision Check
        var memCard1Resolved = _pathResolver.Resolve(settings.Paths.MemoryCard1);
        var memCard2Resolved = _pathResolver.Resolve(settings.Paths.MemoryCard2);

        if (string.Equals(memCard1Resolved, memCard2Resolved, StringComparison.OrdinalIgnoreCase))
        {
            result.AddError($"Memory Card 1 and Memory Card 2 are configured to the exact same directory ('{memCard1Resolved}'). Recommended: Use distinct save directories to prevent save state conflicts.");
        }

        return result;
    }

    public ValidationResult ValidateLaunchReadiness(LauncherSettings settings)
    {
        var result = ValidateSettings(settings);

        if (settings == null) return result;

        var runnerPath = _pathResolver.Resolve(settings.Paths.Runner);
        var dataPath = _pathResolver.Resolve(settings.Paths.Data);
        var elfPath = _pathResolver.Resolve(settings.Paths.Elf);
        var isoPath = _pathResolver.Resolve(settings.Paths.Iso);

        // 1. Runner Executable Check
        if (!_fileSystemService.FileExists(runnerPath))
        {
            result.AddError($"Runner executable missing at '{runnerPath}'. Expected location: bin\\dc2_runner.exe.");
        }
        else
        {
            // Runner DLL Dependency Check
            var runnerDir = Path.GetDirectoryName(runnerPath);
            if (!string.IsNullOrWhiteSpace(runnerDir) && _fileSystemService.DirectoryExists(runnerDir))
            {
                try
                {
                    var dlls = Directory.GetFiles(runnerDir, "*.dll");
                    if (dlls.Length == 0)
                    {
                        result.AddWarning($"Missing Runner Dependencies: No DLL files found in '{runnerDir}'. Game runner may fail to boot due to missing runtime libraries.");
                    }
                }
                catch (Exception ex)
                {
                    result.AddWarning($"Runner Dependency Verification Warning: Could not inspect DLLs in '{runnerDir}': {ex.Message}");
                }
            }
        }

        // 2. DATA Directory & ELF Verification
        if (!_fileSystemService.DirectoryExists(dataPath))
        {
            result.AddError($"DATA directory missing at '{dataPath}'. Recommended Action: Click 'Extract ISO Assets' in launcher.");
        }

        if (!_fileSystemService.FileExists(elfPath))
        {
            result.AddError($"Disc ELF file missing at '{elfPath}'. Recommended Action: Re-extract ISO assets.");
        }

        if (!_fileSystemService.FileExists(isoPath))
        {
            result.AddError($"Game ISO missing at '{isoPath}'. Recommended Action: Place game ISO in launcher folder.");
        }

        return result;
    }
}
