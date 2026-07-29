using System;
using System.IO;
using System.Threading.Tasks;
using DC2Launcher.App.ViewModels;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class MainViewModelTests
{
    private (MainViewModel ViewModel, TestLauncherEnvironment Env, MockFileSystemService Fs) CreateViewModel()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir);

        var env = new TestLauncherEnvironment { LauncherRoot = tempDir };
        var fs = new MockFileSystemService();
        var logger = new MockLoggerService();
        var settingsService = new JsonSettingsService(env, logger);
        var validationService = new ValidationService(env, fs);
        var processLauncher = new ProcessLauncher(env, validationService, logger);
        var isoExtractor = new MockIsoExtractor();

        var vm = new MainViewModel(env, settingsService, fs, processLauncher, validationService, isoExtractor);
        return (vm, env, fs);
    }

    [Fact]
    public void MainViewModel_Initialization_LoadsSettingsAndDefaultValues()
    {
        var (vm, _, _) = CreateViewModel();

        Assert.Equal("Ready", vm.StatusMessage);
        Assert.True(vm.CanPlay);
        Assert.True(vm.CanExtract);
        Assert.True(vm.CanModifySettings);
        Assert.False(vm.IsBusy);
        Assert.False(vm.IsExtracting);
        Assert.Equal("Dark Cloud 2 (USA) (v2.00).iso", vm.IsoFileName);
        Assert.Contains("Not Extracted", vm.DataStatusText);
    }

    [Fact]
    public void MainViewModel_PropertyChange_SavesSettingsAndUpdatesState()
    {
        var (vm, env, _) = CreateViewModel();

        vm.SkipIntro = true;
        vm.Enable60Fps = true;
        vm.EnableDebugMenu = true;
        vm.SelectedResolutionMode = "1920x1080";

        var settingsPath = Path.Combine(env.LauncherRoot, "Config", "launcher_settings.json");
        Assert.True(File.Exists(settingsPath));

        Assert.True(vm.SkipIntro);
        Assert.True(vm.Enable60Fps);
        Assert.True(vm.EnableDebugMenu);
        Assert.Equal("1920x1080", vm.SelectedResolutionMode);
    }

    [Fact]
    public void MainViewModel_IsBusy_DisablesCommandsAndSettingsModification()
    {
        var (vm, _, _) = CreateViewModel();

        vm.IsBusy = true;

        Assert.False(vm.CanPlay);
        Assert.False(vm.CanExtract);
        Assert.False(vm.CanModifySettings);
    }

    [Fact]
    public void MainViewModel_DryRunLaunch_PopulatesDiagnosticStatusMessage()
    {
        var (vm, _, _) = CreateViewModel();

        vm.DryRunLaunchCommand.Execute(null);

        Assert.Contains("Executable:", vm.StatusMessage);
        Assert.Contains("Working Directory:", vm.StatusMessage);
    }

    [Fact]
    public void MainViewModel_OpenLogsFolder_CreatesDirectoryAndOpensExplorer()
    {
        var (vm, env, fs) = CreateViewModel();

        vm.OpenLogsFolderCommand.Execute(null);

        var logsPath = Path.Combine(env.LauncherRoot, "Logs");
        Assert.True(fs.DirectoryExists(logsPath));
        Assert.Contains(logsPath, fs.OpenedFolders);
        Assert.Contains("Opened logs directory", vm.StatusMessage);
    }
}
