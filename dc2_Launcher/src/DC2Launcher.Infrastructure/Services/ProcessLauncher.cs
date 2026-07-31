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

        // The runner reads game content from the EXTRACTED DATA FOLDER, never the ISO.
        // DC2_DATA_DIR is the first rule in the runner's data-source resolution
        // (dc2OpenGameDataSource), so the disc image is only ever needed for extraction.
        config.EnvironmentVariables["DC2_DATA_DIR"] = _pathResolver.Resolve(settings.Paths.Data);

        if (settings.Game.Enable60Fps)
        {
            config.EnvironmentVariables["DC2_PATCH_60FPS"] = "1";
        }

        // Debug Performance: the G447 host check. It measures the shipped default against
        // the full rollback inside one process, so it needs UNCAPPED frames on both arms —
        // DC2 is a locked-30 title and a capped run reads the cap, not a result. Force the
        // patch on here rather than making the user remember to tick two boxes.
        if (settings.Game.DebugPerformance)
        {
            config.EnvironmentVariables["DC2_G447_HOSTCHECK"] = "1";
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

        // Mods Folder Mapping (DC2_DATA_DIR is already set unconditionally above).
        if (settings.Mods.Enabled)
        {
            config.EnvironmentVariables["DC2_MODS_ENABLED"] = "1";
            config.EnvironmentVariables["DC2_MODS_DIR"] = _pathResolver.Resolve(settings.Paths.Mods);
        }

        // Controller Configuration Mapping. Honour the configured path so a user who moves
        // it in settings is not silently ignored; fall back to the standard location.
        var controllerConfigPath = string.IsNullOrWhiteSpace(settings.Paths.ControllerConfig)
            ? Path.Combine(_environment.LauncherRoot, "Config", "controller.json")
            : _pathResolver.Resolve(settings.Paths.ControllerConfig);
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

        // The game now runs entirely off the extracted DATA folder, so its absence is a
        // launch-blocking error rather than a missing-ISO warning.
        var dataDir = _pathResolver.Resolve(settings.Paths.Data);
        if (!Directory.Exists(dataDir))
        {
            result.Errors.Add($"Game data folder not found at '{dataDir}'. Extract the ISO first.");
        }
        else if (!_fileSystemService.FileExists(Path.Combine(dataDir, "DATA.DAT")) ||
                 !_fileSystemService.FileExists(Path.Combine(dataDir, "DATA.HD2")))
        {
            result.Errors.Add(
                $"Game data folder '{dataDir}' is incomplete (DATA.DAT / DATA.HD2 missing). Re-extract the ISO.");
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

                // Debug Performance writes its verdict to the runner's stderr, which is
                // invisible for a windowed launch — capture it to a file the user can find.
                // Only in this mode: redirection is otherwise pure overhead.
                var capturePerfLog = settings.Game.DebugPerformance;
                string perfLogPath = string.Empty;
                StreamWriter? perfLog = null;
                if (capturePerfLog)
                {
                    psi.RedirectStandardError = true;
                    psi.RedirectStandardOutput = true;
                }

                _logger.LogInfo($"Launching process '{psi.FileName}' with working dir '{psi.WorkingDirectory}'.");
                _runningProcess = Process.Start(psi);

                if (_runningProcess == null)
                {
                    errorMessage = "Failed to start process (Process.Start returned null).";
                    _logger.LogError(errorMessage);
                    return false;
                }

                if (capturePerfLog)
                {
                    var logDir = Path.Combine(_environment.LauncherRoot, "Logs");
                    Directory.CreateDirectory(logDir);
                    perfLogPath = Path.Combine(
                        logDir, $"performance_{DateTime.Now:yyyyMMdd_HHmmss}.log");
                    perfLog = new StreamWriter(perfLogPath, append: false) { AutoFlush = true };

                    var sync = new object();
                    void Write(string? line)
                    {
                        if (line == null) return;
                        lock (sync)
                        {
                            try { perfLog?.WriteLine(line); } catch { /* log is best-effort */ }
                        }
                    }

                    _runningProcess.OutputDataReceived += (_, e) => Write(e.Data);
                    _runningProcess.ErrorDataReceived += (_, e) => Write(e.Data);
                    _runningProcess.EnableRaisingEvents = true;
                    _runningProcess.Exited += (_, _) =>
                    {
                        lock (sync)
                        {
                            try { perfLog?.Dispose(); } catch { }
                            perfLog = null;
                        }
                    };
                    _runningProcess.BeginOutputReadLine();
                    _runningProcess.BeginErrorReadLine();
                    _logger.LogInfo($"Debug Performance active. Runner output -> '{perfLogPath}'.");
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
