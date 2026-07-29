using System.Collections.Generic;
using DC2Launcher.Core.Models;

namespace DC2Launcher.Core.Interfaces;

public interface IControllerConfigService
{
    ControllerConfig LoadConfig(string configFilePath);
    void SaveConfig(string configFilePath, ControllerConfig config);
    ControllerConfig GetDefaultConfig();
    List<string> ValidateConfig(ControllerConfig config);
}
