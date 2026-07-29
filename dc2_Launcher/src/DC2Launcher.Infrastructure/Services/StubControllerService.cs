using DC2Launcher.Core.Interfaces;

namespace DC2Launcher.Infrastructure.Services;

public class StubControllerService : IControllerService
{
    public string GetActiveProfileName() => "Default (Keyboard/XInput)";

    public void OpenConfigurationWindow()
    {
        // Controller configuration window to be implemented in Prompt 9
    }
}
