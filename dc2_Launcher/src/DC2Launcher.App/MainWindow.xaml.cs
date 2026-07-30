using System;
using System.Windows;
using System.Windows.Input;
using DC2Launcher.App.Services;
using DC2Launcher.App.ViewModels;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;

namespace DC2Launcher.App;

public partial class MainWindow : Window
{
    private readonly WpfAudioService _audioService;

    public MainWindow()
    {
        InitializeComponent();

        var env = new LauncherEnvironment();
        var logger = new FileLoggerService(env);
        var settingsService = new JsonSettingsService(env, logger);
        var fileService = new FileSystemService();
        var validationService = new ValidationService(env, fileService);
        var processLauncher = new ProcessLauncher(env, fileService, logger);
        var isoExtractor = new DiscUtilsIsoExtractor(env, fileService, logger);

        _audioService = new WpfAudioService();

        DataContext = new MainViewModel(
            env,
            settingsService,
            fileService,
            processLauncher,
            validationService,
            isoExtractor,
            audioService: _audioService);
    }

    protected override void OnClosed(EventArgs e)
    {
        _audioService?.Stop();
        if (DataContext is MainViewModel mainVm)
        {
            mainVm.StopAudio();
        }
        base.OnClosed(e);
    }

    private void Window_KeyDown(object sender, KeyEventArgs e)
    {
        if (DataContext is MainViewModel mainVm && mainVm.ControllerVM.IsCapturingInput)
        {
            mainVm.ControllerVM.HandleKeyDown(e.Key == Key.System ? e.SystemKey : e.Key);
            e.Handled = true;
        }
    }
}