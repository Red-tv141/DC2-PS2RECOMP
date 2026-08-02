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
    private readonly IAudioService? _audioService;
    private readonly PathResolver _pathResolver;

    private LauncherSettings _settings;
    private string _statusMessage = "Ready";
    private bool _isBusy;
    private bool _isExtracting;
    private double _extractionProgress;
    private CancellationTokenSource? _extractionCts;
    private string _activeTab = "Home";

    public MainViewModel(
        ILauncherEnvironment environment,
        ISettingsService settingsService,
        IFileSystemService fileSystemService,
        IProcessLauncher processLauncher,
        IValidationService validationService,
        IIsoExtractor isoExtractor,
        IModScannerService? modScanner = null,
        IAudioService? audioService = null)
    {
        _environment = environment ?? throw new ArgumentNullException(nameof(environment));
        _settingsService = settingsService ?? throw new ArgumentNullException(nameof(settingsService));
        _fileSystemService = fileSystemService ?? throw new ArgumentNullException(nameof(fileSystemService));
        _processLauncher = processLauncher ?? throw new ArgumentNullException(nameof(processLauncher));
        _validationService = validationService ?? throw new ArgumentNullException(nameof(validationService));
        _isoExtractor = isoExtractor ?? throw new ArgumentNullException(nameof(isoExtractor));
        _modScanner = modScanner ?? new ModScannerService(new FileLoggerService(environment));
        _audioService = audioService;
        _pathResolver = new PathResolver(_environment);

        _settings = _settingsService.LoadSettings();
        ControllerVM = new ControllerViewModel(_environment);

        ResolutionModes = new ObservableCollection<string>
        {
            "GameDefault",
            "1280x720",
            "1920x1080",
            "2560x1440",
            "3840x2160",
            "Custom"
        };

        // Navigation Commands
        SelectHomeCommand = new RelayCommand(() => ActiveTab = "Home");
        SelectControlsCommand = new RelayCommand(() => ActiveTab = "Controls");
        SelectSettingsCommand = new RelayCommand(() => ActiveTab = "Settings");
        SelectModsCommand = new RelayCommand(() => ActiveTab = "Mods");
        SelectDiagnosticsCommand = new RelayCommand(() => ActiveTab = "Diagnostics");
        ExitCommand = new RelayCommand(() =>
        {
            StopAudio();
            System.Windows.Application.Current?.Shutdown();
        });

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
        StartBackgroundAudio();
    }

    public ControllerViewModel ControllerVM { get; }

    public string ActiveTab
    {
        get => _activeTab;
        set
        {
            if (SetProperty(ref _activeTab, value))
            {
                OnPropertyChanged(nameof(IsHomeTabActive));
                OnPropertyChanged(nameof(IsControlsTabActive));
                OnPropertyChanged(nameof(IsSettingsTabActive));
                OnPropertyChanged(nameof(IsModsTabActive));
                OnPropertyChanged(nameof(IsDiagnosticsTabActive));
            }
        }
    }

    public bool IsHomeTabActive => ActiveTab == "Home";
    public bool IsControlsTabActive => ActiveTab == "Controls";
    public bool IsSettingsTabActive => ActiveTab == "Settings";
    public bool IsModsTabActive => ActiveTab == "Mods";
    public bool IsDiagnosticsTabActive => ActiveTab == "Diagnostics";

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
                OnPropertyChanged(nameof(CanModifySettings));
            }
        }
    }

    public double ExtractionProgress
    {
        get => _extractionProgress;
        set => SetProperty(ref _extractionProgress, value);
    }

    public string IsoPath
    {
        get => _settings.Paths.Iso;
        set
        {
            if (_settings.Paths.Iso != value)
            {
                _settings.Paths.Iso = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsoFileName));
                OnPropertyChanged(nameof(CanExtract));
                SaveSettings();
            }
        }
    }

    public string IsoFileName
    {
        get
        {
            if (string.IsNullOrWhiteSpace(_settings.Paths.Iso))
                return "No ISO selected (Click Extract ISO to choose)";
            return Path.GetFileName(_settings.Paths.Iso);
        }
    }

    public string DataStatusText
    {
        get
        {
            var dataDir = _pathResolver.Resolve(_settings.Paths.Data);
            if (_fileSystemService.DirectoryExists(dataDir))
            {
                var state = _isoExtractor.GetExtractionState();
                if (state != null && state.Success)
                {
                    var elfName = string.IsNullOrWhiteSpace(state.DetectedElfPath) ? "SCUS_972.13" : Path.GetFileName(state.DetectedElfPath);
                    return $"Extracted ({state.ExtractedTimestampUtc.ToLocalTime():yyyy-MM-dd HH:mm}) — {elfName} Verified";
                }
                return "Extracted DATA/ folder present";
            }
            return "Not Extracted (ISO Required)";
        }
    }

    public ObservableCollection<string> ResolutionModes { get; }

    public string SelectedResolutionMode
    {
        get => _settings.Game.Resolution.Mode;
        set
        {
            if (_settings.Game.Resolution.Mode != value)
            {
                _settings.Game.Resolution.Mode = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsCustomResolutionSelected));
                SaveSettings();
            }
        }
    }

    public bool IsCustomResolutionSelected => SelectedResolutionMode == "Custom";

    public int CustomResolutionWidth
    {
        get => _settings.Game.Resolution.Width ?? 1920;
        set
        {
            if (_settings.Game.Resolution.Width != value)
            {
                _settings.Game.Resolution.Width = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(CustomWidth));
                SaveSettings();
            }
        }
    }

    public int CustomResolutionHeight
    {
        get => _settings.Game.Resolution.Height ?? 1080;
        set
        {
            if (_settings.Game.Resolution.Height != value)
            {
                _settings.Game.Resolution.Height = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(CustomHeight));
                SaveSettings();
            }
        }
    }

    public string? CustomWidth
    {
        get => CustomResolutionWidth.ToString();
        set
        {
            if (int.TryParse(value, out var parsed))
            {
                CustomResolutionWidth = parsed;
            }
        }
    }

    public string? CustomHeight
    {
        get => CustomResolutionHeight.ToString();
        set
        {
            if (int.TryParse(value, out var parsed))
            {
                CustomResolutionHeight = parsed;
            }
        }
    }

    public bool Enable60Fps
    {
        get => _settings.Game.Enable60Fps;
        set
        {
            if (_settings.Game.Enable60Fps != value)
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
            if (_settings.Game.EnableDebugMenu != value)
            {
                _settings.Game.EnableDebugMenu = value;
                OnPropertyChanged();
                SaveSettings();
            }
        }
    }
    public bool SkipIntro
    {
        get => _settings.Game.SkipIntro;
        set
        {
            if (_settings.Game.SkipIntro != value)
            {
                _settings.Game.SkipIntro = value;
                OnPropertyChanged();
                SaveSettings();
            }
        }
    }

    public bool Fullscreen
    {
        get => _settings.Game.Resolution.Fullscreen;
        set
        {
            if (_settings.Game.Resolution.Fullscreen != value)
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
            if (_settings.Mods.Enabled != value)
            {
                _settings.Mods.Enabled = value;
                OnPropertyChanged();
                SaveSettings();
            }
        }
    }

    public string MemoryCard1Path
    {
        get => _settings.Paths.MemoryCard1;
        set
        {
            if (_settings.Paths.MemoryCard1 != value)
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
            if (_settings.Paths.MemoryCard2 != value)
            {
                _settings.Paths.MemoryCard2 = value;
                OnPropertyChanged();
                SaveSettings();
            }
        }
    }

    public bool CanModifySettings => !IsBusy && !IsExtracting;
    public bool CanExtract => !IsBusy && !IsExtracting && !string.IsNullOrWhiteSpace(_settings.Paths.Iso);
    public bool CanPlay => !IsBusy && !IsExtracting;

    public ICommand SelectHomeCommand { get; }
    public ICommand SelectControlsCommand { get; }
    public ICommand SelectSettingsCommand { get; }
    public ICommand SelectModsCommand { get; }
    public ICommand SelectDiagnosticsCommand { get; }
    public ICommand ExitCommand { get; }

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

    public void StopAudio()
    {
        _audioService?.Stop();
    }

    private void StartBackgroundAudio()
    {
        var candidates = new[]
        {
            _pathResolver.Resolve("Launcher_OST.mp3"),
            Path.Combine(AppContext.BaseDirectory, "Launcher_OST.mp3"),
            Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Launcher_OST.mp3"),
        };

        // Also check the directory where the actual exe lives (single-file publish)
        try
        {
            var exePath = System.Diagnostics.Process.GetCurrentProcess().MainModule?.FileName;
            if (!string.IsNullOrEmpty(exePath))
            {
                var exeDir = Path.GetDirectoryName(exePath);
                if (!string.IsNullOrEmpty(exeDir))
                {
                    candidates = candidates.Append(Path.Combine(exeDir, "Launcher_OST.mp3")).ToArray();
                }
            }
        }
        catch { /* ignore */ }

        foreach (var path in candidates)
        {
            if (File.Exists(path))
            {
                _audioService?.PlayLooping(path);
                return;
            }
        }
    }

    private void BrowseIso()
    {
        var dialog = new Microsoft.Win32.OpenFileDialog
        {
            Title = "Select Dark Cloud 2 ISO Disc Image",
            Filter = "ISO Files (*.iso)|*.iso|All Files (*.*)|*.*",
            CheckFileExists = true
        };

        if (dialog.ShowDialog() == true)
        {
            IsoPath = dialog.FileName;
        }
    }

    private async Task ExtractIsoAsync()
    {
        var resolvedIso = _pathResolver.Resolve(_settings.Paths.Iso);
        if (string.IsNullOrWhiteSpace(resolvedIso) || !_fileSystemService.FileExists(resolvedIso))
        {
            BrowseIso();
            resolvedIso = _pathResolver.Resolve(_settings.Paths.Iso);
            if (string.IsNullOrWhiteSpace(resolvedIso) || !_fileSystemService.FileExists(resolvedIso))
            {
                StatusMessage = "Extraction Cancelled — No ISO selected.";
                return;
            }
        }

        try
        {
            IsExtracting = true;
            ExtractionProgress = 0;
            StatusMessage = "Extracting ISO files to DATA/ directory...";
            _extractionCts = new CancellationTokenSource();

            var resolvedData = _pathResolver.Resolve(_settings.Paths.Data);
            Action<double, string> progressCallback = (val, msg) => ExtractionProgress = val;

            var success = await _isoExtractor.ExtractIsoAsync(resolvedIso, resolvedData, progressCallback, _extractionCts.Token);
            if (success)
            {
                StatusMessage = "ISO Extraction Completed Successfully! DATA/ ready.";
                OnPropertyChanged(nameof(DataStatusText));
            }
            else
            {
                StatusMessage = "Extraction incomplete or cancelled.";
            }
        }
        catch (OperationCanceledException)
        {
            StatusMessage = "Extraction process was cancelled.";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Extraction Failed: {ex.Message}";
        }
        finally
        {
            IsExtracting = false;
            _extractionCts?.Dispose();
            _extractionCts = null;
        }
    }

    private void CancelExtraction()
    {
        _extractionCts?.Cancel();
    }

    private void OpenDataFolder()
    {
        var path = _pathResolver.Resolve(_settings.Paths.Data);
        _fileSystemService.CreateDirectory(path);
        _fileSystemService.OpenFolderInExplorer(path);
        StatusMessage = $"Opened DATA directory: {path}";
    }

    private void OpenModsFolder()
    {
        var path = _pathResolver.Resolve(_settings.Paths.Mods);
        _fileSystemService.CreateDirectory(path);
        _fileSystemService.OpenFolderInExplorer(path);
        StatusMessage = $"Opened Mods directory: {path}";
    }

    private void OpenLogsFolder()
    {
        var path = _pathResolver.Resolve("Logs");
        _fileSystemService.CreateDirectory(path);
        _fileSystemService.OpenFolderInExplorer(path);
        StatusMessage = $"Opened logs directory: {path}";
    }

    private void BrowseMemCard1()
    {
        var dialog = new Microsoft.Win32.OpenFolderDialog
        {
            Title = "Select Memory Card Slot 1 Directory",
            InitialDirectory = _pathResolver.Resolve(MemoryCard1Path)
        };

        if (dialog.ShowDialog() == true)
        {
            MemoryCard1Path = _pathResolver.MakeRelativeIfUnderRoot(dialog.FolderName);
        }
    }

    private void BrowseMemCard2()
    {
        var dialog = new Microsoft.Win32.OpenFolderDialog
        {
            Title = "Select Memory Card Slot 2 Directory",
            InitialDirectory = _pathResolver.Resolve(MemoryCard2Path)
        };

        if (dialog.ShowDialog() == true)
        {
            MemoryCard2Path = _pathResolver.MakeRelativeIfUnderRoot(dialog.FolderName);
        }
    }

    private void OpenMemCard1Folder()
    {
        var path = _pathResolver.Resolve(MemoryCard1Path);
        _fileSystemService.CreateDirectory(path);
        _fileSystemService.OpenFolderInExplorer(path);
        StatusMessage = $"Opened Memory Card 1 directory: {path}";
    }

    private void OpenMemCard2Folder()
    {
        var path = _pathResolver.Resolve(MemoryCard2Path);
        _fileSystemService.CreateDirectory(path);
        _fileSystemService.OpenFolderInExplorer(path);
        StatusMessage = $"Opened Memory Card 2 directory: {path}";
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
        ActiveTab = "Controls";
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
            StopAudio();
            IsBusy = true;
            StatusMessage = "Launching Dark Cloud 2 runner...";

            var success = _processLauncher.LaunchGame(_settings, out var errorMessage);
            if (success)
            {
                System.Windows.Application.Current?.Shutdown();
                return;
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
