using System;
using System.IO;
using System.Text.Json;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;

namespace DC2Launcher.Infrastructure.Services;

public class JsonSettingsService : ISettingsService
{
    private readonly ILauncherEnvironment _environment;
    private readonly ILoggerService _logger;
    private readonly PathResolver _pathResolver;
    private readonly string _settingsFilePath;
    private static readonly JsonSerializerOptions JsonOptions = new() 
    { 
        WriteIndented = true, 
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase 
    };

    public JsonSettingsService(ILauncherEnvironment environment, ILoggerService logger)
    {
        _environment = environment ?? throw new ArgumentNullException(nameof(environment));
        _logger = logger ?? throw new ArgumentNullException(nameof(logger));
        _pathResolver = new PathResolver(_environment);
        _settingsFilePath = Path.Combine(_environment.LauncherRoot, "Config", "launcher_settings.json");
    }

    public LauncherSettings LoadSettings()
    {
        try
        {
            if (!File.Exists(_settingsFilePath))
            {
                _logger.LogInfo("Settings file not found. Creating default settings.");
                var defaults = new LauncherSettings();
                SaveSettings(defaults);
                return defaults;
            }

            var json = File.ReadAllText(_settingsFilePath);
            var settings = JsonSerializer.Deserialize<LauncherSettings>(json, JsonOptions);

            if (settings == null)
            {
                _logger.LogWarning("Settings file deserialized to null. Using defaults.");
                return new LauncherSettings();
            }

            NormalizeLoadedPaths(settings);
            return settings;
        }
        catch (Exception ex)
        {
            _logger.LogError("Failed to load settings file. Backing up corrupt settings and restoring defaults.", ex);
            BackupCorruptSettings();
            var defaults = new LauncherSettings();
            SaveSettings(defaults);
            return defaults;
        }
    }

    public void SaveSettings(LauncherSettings settings)
    {
        if (settings == null) throw new ArgumentNullException(nameof(settings));

        try
        {
            NormalizePathsForSaving(settings);

            var configDir = Path.GetDirectoryName(_settingsFilePath)!;
            if (!Directory.Exists(configDir))
            {
                Directory.CreateDirectory(configDir);
            }

            var tempFilePath = _settingsFilePath + ".tmp";
            var json = JsonSerializer.Serialize(settings, JsonOptions);

            // Write to temporary file first
            File.WriteAllText(tempFilePath, json);

            // Atomic replace or move
            if (File.Exists(_settingsFilePath))
            {
                File.Replace(tempFilePath, _settingsFilePath, null);
            }
            else
            {
                File.Move(tempFilePath, _settingsFilePath);
            }

            _logger.LogInfo("Launcher settings saved successfully.");
        }
        catch (Exception ex)
        {
            _logger.LogError("Failed to save settings atomically.", ex);
            throw;
        }
    }

    private void NormalizePathsForSaving(LauncherSettings settings)
    {
        settings.Paths.Iso = _pathResolver.MakeRelativeIfUnderRoot(settings.Paths.Iso);
        settings.Paths.Data = _pathResolver.MakeRelativeIfUnderRoot(settings.Paths.Data);
        settings.Paths.Runner = _pathResolver.MakeRelativeIfUnderRoot(settings.Paths.Runner);
        settings.Paths.Elf = _pathResolver.MakeRelativeIfUnderRoot(settings.Paths.Elf);
        settings.Paths.MemoryCard1 = _pathResolver.MakeRelativeIfUnderRoot(settings.Paths.MemoryCard1);
        settings.Paths.MemoryCard2 = _pathResolver.MakeRelativeIfUnderRoot(settings.Paths.MemoryCard2);
        settings.Paths.Mods = _pathResolver.MakeRelativeIfUnderRoot(settings.Paths.Mods);
        settings.Paths.ControllerConfig = _pathResolver.MakeRelativeIfUnderRoot(settings.Paths.ControllerConfig);
    }

    private void NormalizeLoadedPaths(LauncherSettings settings)
    {
        // Preserve relative paths as stored, but guarantee unnullified defaults
        settings.Paths.Iso ??= "Dark Cloud 2 (USA) (v2.00).iso";
        settings.Paths.Data ??= "DATA";
        settings.Paths.Runner ??= @"bin\dc2_runner.exe";
        settings.Paths.Elf ??= @"DATA\SCUS_972.13";
        settings.Paths.MemoryCard1 ??= @"Saves\MemoryCard1";
        settings.Paths.MemoryCard2 ??= @"Saves\MemoryCard2";
        settings.Paths.Mods ??= "Mods";
        settings.Paths.ControllerConfig ??= @"Config\controller.json";
    }

    private void BackupCorruptSettings()
    {
        try
        {
            if (File.Exists(_settingsFilePath))
            {
                var backupPath = _settingsFilePath + $".corrupt.{DateTime.Now:yyyyMMddHHmmss}.bak";
                File.Move(_settingsFilePath, backupPath);
            }
        }
        catch
        {
            // Best effort backup
        }
    }
}
