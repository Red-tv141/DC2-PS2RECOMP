using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Input;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;

namespace DC2Launcher.App.ViewModels;

public class MainViewModel : ViewModelBase
{
    private readonly ILauncherEnvironment _environment;
    private readonly ISettingsService _settingsService;
    private readonly IFileSystemService _fileSystemService;
    private readonly IProcessLauncher _processLauncher;
    private readonly IValidationService _validationService;
    private readonly IIsoExtractor _isoExtractor;
    private readonly IModScannerService _modScanner;
    private readonly PathResolver _pathResolver;

    private LauncherSettings _settings;
    private string _statusMessage = "Ready";
    private bool _isBusy;
    private bool _isExtracting;
    private double _extractionProgress;
    private CancellationTokenSource? _extractionCts;

    public MainViewModel(
        ILauncherEnvironment environment,
        ISettingsService settingsService,
        IFileSystemService fileSystemService,
        IProcessLauncher processLauncher,
        IValidationService validationService,
        IIsoExtractor isoExtractor,
        IModScannerService? modScanner = null)
    {
        _environment = environment ?? throw new ArgumentNullException(nameof(environment));
        _settingsService = settingsService ?? throw new ArgumentNullException(nameof(settingsService));
        _fileSystemService = fileSystemService ?? throw new ArgumentNullException(nameof(fileSystemService));
        _processLauncher = processLauncher ?? throw new ArgumentNullException(nameof(processLauncher));
        _validationService = validationService ?? throw new ArgumentNullException(nameof(validationService));
        _isoExtractor = isoExtractor ?? throw new ArgumentNullException(nameof(isoExtractor));
        _modScanner = modScanner ?? new ModScannerService(new FileLoggerService(environment));
        _pathResolver = new PathResolver(_environment);

        _settings = _settingsService.LoadSettings();

        ResolutionModes = new ObservableCollection<string>
        {
            "GameDefault",
            "1280x720",
            "1920x1080",
            "2560x1440",
            "3840x2160",
            "Custom"
        };

        BrowseIsoCommand = new RelayCommand(BrowseIso, () => CanModifySettings);
        ExtractIsoCommand = new RelayCommand(async () => await ExtractIsoAsync(), () => CanExtract);
        CancelExtractionCommand = new RelayCommand(CancelExtraction, () => IsExtracting);
        OpenDataFolderCommand = new RelayCommand(OpenDataFolder);
        OpenModsFolderCommand = new RelayCommand(OpenModsFolder);
        OpenLogsFolderCommand = new RelayCommand(OpenLogsFolder);

        BrowseMemCard1Command = new RelayCommand(BrowseMemCard1, () => CanModifySettings);
        BrowseMemCard2Command = new RelayCommand(BrowseMemCard2, () => CanModifySettings);
        OpenMemCard1FolderCommand = new RelayCommand(OpenMemCard1Folder);
        OpenMemCard2FolderCommand = new RelayCommand(OpenMemCard2Folder);
        ResetMemCard1Command = new RelayCommand(ResetMemCard1, () => CanModifySettings);
        ResetMemCard2Command = new RelayCommand(ResetMemCard2, () => CanModifySettings);

        CustomizeControllerCommand = new RelayCommand(CustomizeController, () => CanModifySettings);
        ValidateLaunchCommand = new RelayCommand(ValidateLaunch);
        DryRunLaunchCommand = new RelayCommand(DryRunLaunch, () => !IsBusy);
        PlayCommand = new RelayCommand(Play, () => CanPlay);

        ValidateCurrentSettings();
    }

    public ObservableCollection<string> ResolutionModes { get; }

    public bool SkipIntro
    {
        get => _settings.Game.SkipIntro;
        set
        {
            if (_settings.Game.SkipIntro != value && CanModifySettings)
            {
                _settings.Game.SkipIntro = value;
                OnPropertyChanged();
                SaveSettings();
            }
        }
    }

    public bool Enable60Fps
    {
        get => _settings.Game.Enable60Fps;
        set
        {
            if (_settings.Game.Enable60Fps != value && CanModifySettings)
            {
                _settings.Game.Enable60Fps = value;
                OnPropertyChanged();
                SaveSettings();
            }
        }
    }

    public bool EnableDebugMenu
    {
        get => _settings.Game.EnableDebugMenu;
        set
        {
            if (_settings.Game.EnableDebugMenu != value && CanModifySettings)
            {
                _settings.Game.EnableDebugMenu = value;
                OnPropertyChanged();
                SaveSettings();
            }
        }
    }

    public string SelectedResolutionMode
    {
        get => _settings.Game.Resolution.Mode;
        set
        {
            if (_settings.Game.Resolution.Mode != value && CanModifySettings)
            {
                _settings.Game.Resolution.Mode = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsCustomResolutionSelected));
                SaveSettings();
            }
        }
    }

    public bool IsCustomResolutionSelected => string.Equals(SelectedResolutionMode, "Custom", StringComparison.OrdinalIgnoreCase);

    public string CustomResolutionWidth
    {
        get => _settings.Game.Resolution.Width?.ToString() ?? string.Empty;
        set
        {
            if (CanModifySettings)
            {
                if (int.TryParse(value, out var parsed) && parsed > 0)
                {
                    _settings.Game.Resolution.Width = parsed;
                }
                else if (string.IsNullOrWhiteSpace(value))
                {
                    _settings.Game.Resolution.Width = null;
                }
                OnPropertyChanged();
                OnPropertyChanged(nameof(CustomWidth));
                SaveSettings();
            }
        }
    }

    public string CustomResolutionHeight
    {
        get => _settings.Game.Resolution.Height?.ToString() ?? string.Empty;
        set
        {
            if (CanModifySettings)
            {
                if (int.TryParse(value, out var parsed) && parsed > 0)
                {
                    _settings.Game.Resolution.Height = parsed;
                }
                else if (string.IsNullOrWhiteSpace(value))
                {
                    _settings.Game.Resolution.Height = null;
                }
                OnPropertyChanged();
                OnPropertyChanged(nameof(CustomHeight));
                SaveSettings();
            }
        }
    }

    public string CustomWidth
    {
        get => CustomResolutionWidth;
        set => CustomResolutionWidth = value;
    }

    public string CustomHeight
    {
        get => CustomResolutionHeight;
        set => CustomResolutionHeight = value;
    }

    public bool Fullscreen
    {
        get => _settings.Game.Resolution.Fullscreen;
        set
        {
            if (_settings.Game.Resolution.Fullscreen != value && CanModifySettings)
            {
                _settings.Game.Resolution.Fullscreen = value;
                OnPropertyChanged();
                SaveSettings();
            }
        }
    }

    public bool EnableModsFolder
    {
        get => _settings.Mods.Enabled;
        set
        {
            if (_settings.Mods.Enabled != value && CanModifySettings)
            {
                _settings.Mods.Enabled = value;
                OnPropertyChanged();
                SaveSettings();

                if (value)
                {
                    ScanModsAndSaveManifest();
                }
            }
        }
    }

    public string MemoryCard1Path
    {
        get => _settings.Paths.MemoryCard1;
        set
        {
            if (_settings.Paths.MemoryCard1 != value && CanModifySettings)
            {
                _settings.Paths.MemoryCard1 = value;
                OnPropertyChanged();
                SaveSettings();
            }
        }
    }

    public string MemoryCard2Path
    {
        get => _settings.Paths.MemoryCard2;
        set
        {
            if (_settings.Paths.MemoryCard2 != value && CanModifySettings)
            {
                _settings.Paths.MemoryCard2 = value;
                OnPropertyChanged();
                SaveSettings();
            }
        }
    }

    public string IsoFileName => _settings.Paths.Iso;

    public string IsoPathInput
    {
        get => _settings.Paths.Iso;
        set
        {
            if (_settings.Paths.Iso != value && CanModifySettings)
            {
                var relative = _pathResolver.MakeRelativeIfUnderRoot(value);
                _settings.Paths.Iso = relative;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsoFileName));
                SaveSettings();
            }
        }
    }

    public string DataStatusText
    {
        get
        {
            var isPopulated = _isoExtractor.IsDataPopulated(_settings.Paths.Data);
            if (isPopulated)
            {
                var state = _isoExtractor.GetExtractionState();
                if (state != null)
                {
                    return $"Extracted ({state.ExtractedTimestampUtc.ToLocalTime():yyyy-MM-dd HH:mm}) — ELF Verified";
                }
                return "Ready — SCUS_972.13 Verified";
            }
            return "Not Extracted (DATA folder or SCUS_972.13 missing)";
        }
    }

    public string StatusMessage
    {
        get => _statusMessage;
        set => SetProperty(ref _statusMessage, value);
    }

    public bool IsBusy
    {
        get => _isBusy;
        set
        {
            if (SetProperty(ref _isBusy, value))
            {
                OnPropertyChanged(nameof(CanModifySettings));
                OnPropertyChanged(nameof(CanExtract));
                OnPropertyChanged(nameof(CanPlay));
            }
        }
    }

    public bool IsExtracting
    {
        get => _isExtracting;
        set
        {
            if (SetProperty(ref _isExtracting, value))
            {
                OnPropertyChanged(nameof(CanExtract));
                OnPropertyChanged(nameof(CanPlay));
            }
        }
    }

    public double ExtractionProgress
    {
        get => _extractionProgress;
        set => SetProperty(ref _extractionProgress, value);
    }

    public bool CanModifySettings => !IsBusy && !IsExtracting;
    public bool CanExtract => !IsExtracting && !IsBusy;
    public bool CanPlay => !IsExtracting && !IsBusy;

    public ICommand BrowseIsoCommand { get; }
    public ICommand ExtractIsoCommand { get; }
    public ICommand CancelExtractionCommand { get; }
    public ICommand OpenDataFolderCommand { get; }
    public ICommand OpenModsFolderCommand { get; }
    public ICommand OpenLogsFolderCommand { get; }

    public ICommand BrowseMemCard1Command { get; }
    public ICommand BrowseMemCard2Command { get; }
    public ICommand OpenMemCard1FolderCommand { get; }
    public ICommand OpenMemCard2FolderCommand { get; }
    public ICommand ResetMemCard1Command { get; }
    public ICommand ResetMemCard2Command { get; }

    public ICommand CustomizeControllerCommand { get; }
    public ICommand ValidateLaunchCommand { get; }
    public ICommand DryRunLaunchCommand { get; }
    public ICommand PlayCommand { get; }

    private void BrowseIso()
    {
        try
        {
            var dialog = new Microsoft.Win32.OpenFileDialog
            {
                Title = "Select Dark Cloud 2 ISO Image",
                Filter = "PS2 ISO Image (*.iso)|*.iso|All Files (*.*)|*.*"
            };

            var resolvedCurrent = _pathResolver.Resolve(_settings.Paths.Iso);
            if (_fileSystemService.FileExists(resolvedCurrent))
            {
                dialog.InitialDirectory = Path.GetDirectoryName(resolvedCurrent);
                dialog.FileName = Path.GetFileName(resolvedCurrent);
            }
            else
            {
                dialog.InitialDirectory = _environment.LauncherRoot;
            }

            if (dialog.ShowDialog() == true)
            {
                IsoPathInput = dialog.FileName;
                StatusMessage = $"Selected ISO Image: {dialog.FileName}";
            }
        }
        catch (Exception ex)
        {
            StatusMessage = $"Error selecting ISO file: {ex.Message}";
        }
    }

    private async Task ExtractIsoAsync()
    {
        if (IsExtracting) return;

        var isoPath = _pathResolver.Resolve(_settings.Paths.Iso);
        var dataPath = _pathResolver.Resolve(_settings.Paths.Data);

        if (!_fileSystemService.FileExists(isoPath))
        {
            try
            {
                var dialog = new Microsoft.Win32.OpenFileDialog
                {
                    Title = "Select Dark Cloud 2 ISO Image to Extract",
                    Filter = "PS2 ISO Image (*.iso)|*.iso|All Files (*.*)|*.*",
                    InitialDirectory = _environment.LauncherRoot
                };

                if (dialog.ShowDialog() == true)
                {
                    IsoPathInput = dialog.FileName;
                    isoPath = _pathResolver.Resolve(_settings.Paths.Iso);
                }
                else
                {
                    StatusMessage = "ISO Extraction cancelled: No ISO file selected.";
                    return;
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Failed to open ISO file selector: {ex.Message}";
                return;
            }
        }

        IsExtracting = true;
        IsBusy = true;
        ExtractionProgress = 0;
        _extractionCts = new CancellationTokenSource();

        try
        {
            var success = await _isoExtractor.ExtractIsoAsync(
                isoPath,
                dataPath,
                (percent, message) =>
                {
                    ExtractionProgress = percent;
                    StatusMessage = $"[{percent:F1}%] {message}";
                },
                _extractionCts.Token);

            if (success)
            {
                OnPropertyChanged(nameof(DataStatusText));
                StatusMessage = "ISO Extraction finished cleanly! DATA folder and SCUS_972.13 verified.";
            }
        }
        finally
        {
            IsExtracting = false;
            IsBusy = false;
            _extractionCts?.Dispose();
            _extractionCts = null;
        }
    }

    private void CancelExtraction()
    {
        if (IsExtracting && _extractionCts != null)
        {
            StatusMessage = "Cancelling ISO extraction...";
            _extractionCts.Cancel();
        }
    }

    private void OpenDataFolder()
    {
        var dataPath = _pathResolver.Resolve(_settings.Paths.Data);
        _fileSystemService.CreateDirectory(dataPath);
        _fileSystemService.OpenFolderInExplorer(dataPath);
    }

    private void OpenModsFolder()
    {
        var modsPath = _pathResolver.Resolve(_settings.Paths.Mods);
        _fileSystemService.CreateDirectory(modsPath);
        ScanModsAndSaveManifest();
        _fileSystemService.OpenFolderInExplorer(modsPath);
    }

    private void ScanModsAndSaveManifest()
    {
        var modsPath = _pathResolver.Resolve(_settings.Paths.Mods);
        _fileSystemService.CreateDirectory(modsPath);

        var scanResult = _modScanner.ScanModsDirectory(modsPath);
        var manifestPath = Path.Combine(_environment.LauncherRoot, "Config", "mods_manifest.json");
        _modScanner.SaveManifest(manifestPath, scanResult);

        if (scanResult.Warnings.Count > 0)
        {
            StatusMessage = $"Mods Scanned: Discovered {scanResult.ModFiles.Count} mod file(s) with {scanResult.Warnings.Count} warning(s):\n• " +
                            string.Join("\n• ", scanResult.Warnings);
        }
        else
        {
            StatusMessage = $"Mods Scanned: Discovered {scanResult.ModFiles.Count} mod file(s). Manifest saved to Config\\mods_manifest.json.";
        }
    }

    private void OpenLogsFolder()
    {
        var logsPath = Path.Combine(_environment.LauncherRoot, "Logs");
        _fileSystemService.CreateDirectory(logsPath);
        _fileSystemService.OpenFolderInExplorer(logsPath);
        StatusMessage = $"Opened logs directory: {logsPath}";
    }

    private void BrowseMemCard1()
    {
        try
        {
            var dialog = new Microsoft.Win32.OpenFolderDialog
            {
                Title = "Select Memory Card 1 Save Directory",
                InitialDirectory = _pathResolver.Resolve(_settings.Paths.MemoryCard1)
            };

            if (dialog.ShowDialog() == true)
            {
                MemoryCard1Path = dialog.FolderName;
            }
        }
        catch (Exception ex)
        {
            StatusMessage = $"Error selecting Memory Card 1 directory: {ex.Message}";
        }
    }

    private void BrowseMemCard2()
    {
        try
        {
            var dialog = new Microsoft.Win32.OpenFolderDialog
            {
                Title = "Select Memory Card 2 Save Directory",
                InitialDirectory = _pathResolver.Resolve(_settings.Paths.MemoryCard2)
            };

            if (dialog.ShowDialog() == true)
            {
                MemoryCard2Path = dialog.FolderName;
            }
        }
        catch (Exception ex)
        {
            StatusMessage = $"Error selecting Memory Card 2 directory: {ex.Message}";
        }
    }

    private void OpenMemCard1Folder()
    {
        var dir = _pathResolver.Resolve(_settings.Paths.MemoryCard1);
        _fileSystemService.CreateDirectory(dir);
        _fileSystemService.OpenFolderInExplorer(dir);
    }

    private void OpenMemCard2Folder()
    {
        var dir = _pathResolver.Resolve(_settings.Paths.MemoryCard2);
        _fileSystemService.CreateDirectory(dir);
        _fileSystemService.OpenFolderInExplorer(dir);
    }

    private void ResetMemCard1()
    {
        MemoryCard1Path = @"Saves\MemoryCard1";
        StatusMessage = "Memory Card 1 path reset to default: Saves\\MemoryCard1";
    }

    private void ResetMemCard2()
    {
        MemoryCard2Path = @"Saves\MemoryCard2";
        StatusMessage = "Memory Card 2 path reset to default: Saves\\MemoryCard2";
    }

    private void CustomizeController()
    {
        try
        {
            var vm = new ControllerViewModel(_environment);
            var window = new ControllerWindow(vm)
            {
                Owner = System.Windows.Application.Current.MainWindow
            };
            window.ShowDialog();
            StatusMessage = "Controller configuration saved to Config\\controller.json.";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Error opening controller configuration dialog: {ex.Message}";
        }
    }

    private void ValidateLaunch()
    {
        ValidateCurrentSettings();
    }

    private void DryRunLaunch()
    {
        try
        {
            var config = _processLauncher.BuildLaunchConfiguration(_settings);
            StatusMessage = config.FormatDiagnosticSummary();
        }
        catch (Exception ex)
        {
            StatusMessage = $"Dry Run Failed: {ex.Message}";
        }
    }

    private void Play()
    {
        var readiness = _validationService.ValidateLaunchReadiness(_settings);
        if (!readiness.IsValid)
        {
            StatusMessage = "Cannot Launch — Missing Requirements:\n• " + string.Join("\n• ", readiness.Errors);
            return;
        }

        try
        {
            IsBusy = true;
            StatusMessage = "Launching Dark Cloud 2 runner...";

            var success = _processLauncher.LaunchGame(_settings, out var errorMessage);
            if (success)
            {
                StatusMessage = "Game Launched Successfully! Runner process active.";
            }
            else
            {
                StatusMessage = $"Launch Failed: {errorMessage}";
            }
        }
        catch (Exception ex)
        {
            StatusMessage = $"Launch Exception: {ex.Message}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    private void SaveSettings()
    {
        _settingsService.SaveSettings(_settings);
        ValidateCurrentSettings();
    }

    private void ValidateCurrentSettings()
    {
        var result = _validationService.ValidateSettings(_settings);
        if (!result.IsValid)
        {
            StatusMessage = "Launch Validation Issues:\n• " + string.Join("\n• ", result.Errors);
        }
        else if (result.Warnings.Count > 0)
        {
            StatusMessage = "Configuration Warnings:\n• " + string.Join("\n• ", result.Warnings);
        }
    }
}
