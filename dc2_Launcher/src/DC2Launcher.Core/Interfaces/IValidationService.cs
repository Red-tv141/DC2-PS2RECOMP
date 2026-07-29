using DC2Launcher.Core.Models;

namespace DC2Launcher.Core.Interfaces;

public interface IValidationService
{
    ValidationResult ValidateSettings(LauncherSettings settings);
    ValidationResult ValidateLaunchReadiness(LauncherSettings settings);
}
