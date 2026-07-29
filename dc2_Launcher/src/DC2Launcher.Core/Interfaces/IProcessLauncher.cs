using DC2Launcher.Core.Models;

namespace DC2Launcher.Core.Interfaces;

public interface IProcessLauncher
{
    bool IsGameRunning { get; }
    LaunchConfiguration BuildLaunchConfiguration(LauncherSettings settings);
    ValidationResult ValidateLaunch(LauncherSettings settings);
    bool LaunchGame(LauncherSettings settings, out string errorMessage);
}
