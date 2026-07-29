using System.Windows;
using DC2Launcher.App.ViewModels;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;

namespace DC2Launcher.App;

public partial class MainWindow : Window
{
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

        DataContext = new MainViewModel(env, settingsService, fileService, processLauncher, validationService, isoExtractor);
    }
}