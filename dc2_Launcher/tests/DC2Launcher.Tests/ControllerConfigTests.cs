using System;
using System.IO;
using DC2Launcher.Core.Models;
using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class ControllerConfigTests
{
    [Fact]
    public void ControllerConfigService_GetDefaultConfig_ContainsAll24Ps2Controls()
    {
        var logger = new MockLoggerService();
        var service = new ControllerConfigService(logger);

        var defaults = service.GetDefaultConfig();

        Assert.NotNull(defaults);
        Assert.Equal(1, defaults.SchemaVersion);
        Assert.Equal(10.0, defaults.LeftStickDeadZonePercent);
        Assert.Equal(10.0, defaults.RightStickDeadZonePercent);

        var totalPs2Buttons = Enum.GetValues(typeof(Ps2Button)).Length;
        Assert.Equal(totalPs2Buttons, defaults.Mappings.Count);

        foreach (Ps2Button btn in Enum.GetValues(typeof(Ps2Button)))
        {
            Assert.True(defaults.Mappings.ContainsKey(btn.ToString()), $"Missing default mapping for PS2 control '{btn}'");
        }
    }

    [Fact]
    public void ControllerConfigService_SaveAndLoad_PreservesMappingsAndDeadZones()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir);

        try
        {
            var logger = new MockLoggerService();
            var service = new ControllerConfigService(logger);

            var config = service.GetDefaultConfig();
            config.LeftStickDeadZonePercent = 15.0;
            config.RightStickDeadZonePercent = 20.0;
            config.Mappings[Ps2Button.Cross.ToString()] = new InputBinding
            {
                DeviceType = "Keyboard",
                KeyOrButton = "KeyZ",
                DisplayName = "Key Z"
            };

            var configPath = Path.Combine(tempDir, "Config", "controller.json");
            service.SaveConfig(configPath, config);

            Assert.True(File.Exists(configPath));

            var loaded = service.LoadConfig(configPath);
            Assert.NotNull(loaded);
            Assert.Equal(15.0, loaded.LeftStickDeadZonePercent);
            Assert.Equal(20.0, loaded.RightStickDeadZonePercent);
            Assert.Equal("KeyZ", loaded.Mappings[Ps2Button.Cross.ToString()].KeyOrButton);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }

    [Fact]
    public void ControllerConfigService_ValidateConfig_DetectsDuplicateBindings()
    {
        var logger = new MockLoggerService();
        var service = new ControllerConfigService(logger);

        var config = service.GetDefaultConfig();
        // Set both Cross and Circle to the same key
        config.Mappings[Ps2Button.Cross.ToString()].KeyOrButton = "Space";
        config.Mappings[Ps2Button.Circle.ToString()].KeyOrButton = "Space";

        var issues = service.ValidateConfig(config);

        Assert.NotEmpty(issues);
        Assert.Contains(issues, i => i.Contains("Duplicate Binding", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void ControllerConfigService_ValidateConfig_ValidatesDeadZoneRanges()
    {
        var logger = new MockLoggerService();
        var service = new ControllerConfigService(logger);

        var config = service.GetDefaultConfig();
        config.LeftStickDeadZonePercent = -5.0;

        var issues = service.ValidateConfig(config);

        Assert.NotEmpty(issues);
        Assert.Contains(issues, i => i.Contains("Left Stick Dead Zone", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void ProcessLauncher_BuildLaunchConfiguration_EmitsControllerConfigEnvironmentVariable()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var logger = new MockLoggerService();
        var launcher = new ProcessLauncher(env, fs, logger);

        var settings = new LauncherSettings();
        var launchConfig = launcher.BuildLaunchConfiguration(settings);

        Assert.True(launchConfig.EnvironmentVariables.ContainsKey("DC2_CONTROLLER_CONFIG"));
        var expectedPath = Path.Combine(env.LauncherRoot, "Config", "controller.json");
        Assert.Equal(expectedPath, launchConfig.EnvironmentVariables["DC2_CONTROLLER_CONFIG"]);
    }
}
