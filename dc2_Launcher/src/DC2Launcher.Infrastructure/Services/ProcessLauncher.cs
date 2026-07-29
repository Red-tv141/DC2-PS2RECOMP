using System;
using System.Diagnostics;
using System.IO;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;

namespace DC2Launcher.Infrastructure.Services;

public class ProcessLauncher : IProcessLauncher
{
    private readonly ILauncherEnvironment _environment;
    private readonly IFileSystemService _fileSystemService;
    private readonly ILoggerService _logger;
    private readonly PathResolver _pathResolver;
    private readonly object _processLock = new();

    private Process? _runningProcess;

    public ProcessLauncher(
        ILauncherEnvironment environment,
        IValidationService validationService,
        ILoggerService logger)
        : this(environment, new FileSystemService(), logger)
    {
    }

    public ProcessLauncher(
        ILauncherEnvironment environment,
        IFileSystemService fileSystemService,
        ILoggerService logger)
    {
        _environment = environment ?? throw new ArgumentNullException(nameof(environment));
        _fileSystemService = fileSystemService ?? throw new ArgumentNullException(nameof(fileSystemService));
        _logger = logger ?? throw new ArgumentNullException(nameof(logger));
        _pathResolver = new PathResolver(_environment);
    }

    public bool IsGameRunning
    {
        get
        {
            lock (_processLock)
            {
                if (_runningProcess == null) return false;
                try
                {
                    return !_runningProcess.HasExited;
                }
                catch
                {
                    return false;
                }
            }
        }
    }

    public LaunchConfiguration BuildLaunchConfiguration(LauncherSettings settings)
    {
        if (settings == null) throw new ArgumentNullException(nameof(settings));

        var runnerPath = _pathResolver.Resolve(settings.Paths.Runner);
        var elfPath = _pathResolver.Resolve(settings.Paths.Elf);

        var config = new LaunchConfiguration
        {
            ExecutablePath = runnerPath,
            WorkingDirectory = _environment.LauncherRoot
        };

        config.Arguments.Add(elfPath);

        if (settings.Game.Enable60Fps)
        {
            config.EnvironmentVariables["DC2_PATCH_60FPS"] = "1";
        }

        if (settings.Game.EnableDebugMenu)
        {
            config.EnvironmentVariables["DC2_DEBUG_MENU"] = "1";
        }

        if (settings.Game.SkipIntro)
        {
            config.EnvironmentVariables["DC2_SKIP_INTRO"] = "1";
        }

        var memCard1Dir = _pathResolver.Resolve(settings.Paths.MemoryCard1);
        var memCard2Dir = _pathResolver.Resolve(settings.Paths.MemoryCard2);
        config.EnvironmentVariables["DC2_MEMCARD1_DIR"] = memCard1Dir;
        config.EnvironmentVariables["DC2_MEMCARD2_DIR"] = memCard2Dir;

        // Resolution preset & custom mapping
        var mode = settings.Game.Resolution.Mode;
        if (!string.Equals(mode, "GameDefault", StringComparison.OrdinalIgnoreCase))
        {
            if (string.Equals(mode, "Custom", StringComparison.OrdinalIgnoreCase))
            {
                if (settings.Game.Resolution.Width.HasValue && settings.Game.Resolution.Width.Value > 0)
                {
                    config.EnvironmentVariables["DC2_RESOLUTION_WIDTH"] = settings.Game.Resolution.Width.Value.ToString();
                }
                if (settings.Game.Resolution.Height.HasValue && settings.Game.Resolution.Height.Value > 0)
                {
                    config.EnvironmentVariables["DC2_RESOLUTION_HEIGHT"] = settings.Game.Resolution.Height.Value.ToString();
                }
            }
            else
            {
                var parts = mode.Split('x');
                if (parts.Length == 2 && int.TryParse(parts[0], out var w) && int.TryParse(parts[1], out var h))
                {
                    config.EnvironmentVariables["DC2_RESOLUTION_WIDTH"] = w.ToString();
                    config.EnvironmentVariables["DC2_RESOLUTION_HEIGHT"] = h.ToString();
                }
            }
        }

        if (settings.Game.Resolution.Fullscreen)
        {
            config.EnvironmentVariables["DC2_FULLSCREEN"] = "1";
        }

        // Mods Folder Mapping
        if (settings.Mods.Enabled)
        {
            config.EnvironmentVariables["DC2_MODS_ENABLED"] = "1";
            config.EnvironmentVariables["DC2_MODS_DIR"] = _pathResolver.Resolve(settings.Paths.Mods);
            config.EnvironmentVariables["DC2_DATA_DIR"] = _pathResolver.Resolve(settings.Paths.Data);
        }

        // Controller Configuration Mapping
        var controllerConfigPath = Path.Combine(_environment.LauncherRoot, "Config", "controller.json");
        config.EnvironmentVariables["DC2_CONTROLLER_CONFIG"] = controllerConfigPath;

        return config;
    }

    public ValidationResult ValidateLaunch(LauncherSettings settings)
    {
        var config = BuildLaunchConfiguration(settings);
        var result = new ValidationResult();

        if (!_fileSystemService.FileExists(config.ExecutablePath))
        {
            result.Errors.Add($"Runner executable not found at '{config.ExecutablePath}'.");
        }

        if (config.Arguments.Count > 0 && !_fileSystemService.FileExists(config.Arguments[0]))
        {
            result.Errors.Add($"Game ELF executable not found at '{config.Arguments[0]}'.");
        }

        return result;
    }

    public bool LaunchGame(LauncherSettings settings, out string errorMessage)
    {
        errorMessage = string.Empty;

        lock (_processLock)
        {
            if (IsGameRunning)
            {
                errorMessage = "Game process is already running.";
                _logger.LogWarning(errorMessage);
                return false;
            }

            var launchConfig = BuildLaunchConfiguration(settings);

            if (!_fileSystemService.FileExists(launchConfig.ExecutablePath))
            {
                errorMessage = $"Runner executable not found at '{launchConfig.ExecutablePath}'.";
                _logger.LogError(errorMessage);
                return false;
            }

            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = launchConfig.ExecutablePath,
                    WorkingDirectory = launchConfig.WorkingDirectory,
                    UseShellExecute = false
                };

                foreach (var arg in launchConfig.Arguments)
                {
                    psi.ArgumentList.Add(arg);
                }

                foreach (var kvp in launchConfig.EnvironmentVariables)
                {
                    psi.EnvironmentVariables[kvp.Key] = kvp.Value;
                }

                _logger.LogInfo($"Launching process '{psi.FileName}' with working dir '{psi.WorkingDirectory}'.");
                _runningProcess = Process.Start(psi);

                if (_runningProcess == null)
                {
                    errorMessage = "Failed to start process (Process.Start returned null).";
                    _logger.LogError(errorMessage);
                    return false;
                }

                _logger.LogInfo($"Game runner process started with PID {_runningProcess.Id}.");
                return true;
            }
            catch (Exception ex)
            {
                errorMessage = $"Exception starting game process: {ex.Message}";
                _logger.LogError(errorMessage, ex);
                return false;
            }
        }
    }
}
