using System.IO;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class ProcessLauncherTests
{
    [Fact]
    public void BuildLaunchConfiguration_DisabledOptions_OmitsEnvironmentVariables()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var logger = new MockLoggerService();
        var launcher = new ProcessLauncher(env, validator, logger);

        var settings = new LauncherSettings();
        settings.Game.EnableDebugMenu = false;
        settings.Game.Enable60Fps = false;

        var config = launcher.BuildLaunchConfiguration(settings);

        Assert.Equal(Path.GetFullPath(@"C:\MockLauncher\bin\dc2_runner.exe"), config.ExecutablePath);
        Assert.Equal(@"C:\MockLauncher", config.WorkingDirectory);
        Assert.Single(config.Arguments);
        Assert.Equal(Path.GetFullPath(@"C:\MockLauncher\DATA\SCUS_972.13"), config.Arguments[0]);

        Assert.False(config.EnvironmentVariables.ContainsKey("DC2_DEBUG_MENU"));
        Assert.False(config.EnvironmentVariables.ContainsKey("DC2_PATCH_60FPS"));
    }

    [Fact]
    public void BuildLaunchConfiguration_EnabledKnownOptions_SetsExpectedEnvironmentVariables()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var logger = new MockLoggerService();
        var launcher = new ProcessLauncher(env, validator, logger);

        var settings = new LauncherSettings();
        settings.Game.EnableDebugMenu = true;
        settings.Game.Enable60Fps = true;

        var config = launcher.BuildLaunchConfiguration(settings);

        Assert.True(config.EnvironmentVariables.ContainsKey("DC2_DEBUG_MENU"));
        Assert.Equal("1", config.EnvironmentVariables["DC2_DEBUG_MENU"]);

        Assert.True(config.EnvironmentVariables.ContainsKey("DC2_PATCH_60FPS"));
        Assert.Equal("1", config.EnvironmentVariables["DC2_PATCH_60FPS"]);
    }

    [Fact]
    public void BuildLaunchConfiguration_PathWithSpaces_ResolvesCleanlyInArgumentsAndEnvironment()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\Games\Dark Cloud 2 Recomp" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var logger = new MockLoggerService();
        var launcher = new ProcessLauncher(env, validator, logger);

        var settings = new LauncherSettings();
        settings.Paths.Runner = @"bin\dc2_runner.exe";
        settings.Paths.Elf = @"DATA\SCUS_972.13";

        var config = launcher.BuildLaunchConfiguration(settings);

        Assert.Equal(Path.GetFullPath(@"C:\Games\Dark Cloud 2 Recomp\bin\dc2_runner.exe"), config.ExecutablePath);
        Assert.Equal(@"C:\Games\Dark Cloud 2 Recomp", config.WorkingDirectory);
        Assert.Equal(Path.GetFullPath(@"C:\Games\Dark Cloud 2 Recomp\DATA\SCUS_972.13"), config.Arguments[0]);
    }

    [Fact]
    public void LaunchConfiguration_FormatDiagnosticSummary_ProducesReadableSummary()
    {
        var config = new LaunchConfiguration
        {
            ExecutablePath = @"C:\Launcher\bin\dc2_runner.exe",
            WorkingDirectory = @"C:\Launcher"
        };
        config.Arguments.Add(@"C:\Launcher\DATA\SCUS_972.13");
        config.EnvironmentVariables["DC2_PATCH_60FPS"] = "1";

        var summary = config.FormatDiagnosticSummary();

        Assert.Contains(@"Executable: C:\Launcher\bin\dc2_runner.exe", summary);
        Assert.Contains(@"Working Directory: C:\Launcher", summary);
        Assert.Contains(@"Arguments: C:\Launcher\DATA\SCUS_972.13", summary);
        Assert.Contains(@"DC2_PATCH_60FPS = 1", summary);
    }
}
