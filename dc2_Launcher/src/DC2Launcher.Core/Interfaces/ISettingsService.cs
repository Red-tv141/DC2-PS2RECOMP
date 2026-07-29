using DC2Launcher.Core.Models;

namespace DC2Launcher.Core.Interfaces;

public interface ISettingsService
{
    LauncherSettings LoadSettings();
    void SaveSettings(LauncherSettings settings);
}
