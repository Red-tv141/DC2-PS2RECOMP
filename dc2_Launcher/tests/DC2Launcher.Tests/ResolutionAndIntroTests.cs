using System.IO;
using DC2Launcher.App.ViewModels;
using DC2Launcher.Core.Models;
using DC2Launcher.Core.Services;
using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class ResolutionAndIntroTests
{
    [Fact]
    public void ProcessLauncher_SkipIntro_EmitsOrOmitsVariable()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var logger = new MockLoggerService();
        var launcher = new ProcessLauncher(env, validator, logger);

        var settings = new LauncherSettings();

        // Disabled by default
        settings.Game.SkipIntro = false;
        var configDisabled = launcher.BuildLaunchConfiguration(settings);
        Assert.False(configDisabled.EnvironmentVariables.ContainsKey("DC2_SKIP_INTRO"));

        // Enabled
        settings.Game.SkipIntro = true;
        var configEnabled = launcher.BuildLaunchConfiguration(settings);
        Assert.True(configEnabled.EnvironmentVariables.ContainsKey("DC2_SKIP_INTRO"));
        Assert.Equal("1", configEnabled.EnvironmentVariables["DC2_SKIP_INTRO"]);
    }

    [Theory]
    [InlineData("GameDefault", null, null)]
    [InlineData("1280x720", "1280", "720")]
    [InlineData("1920x1080", "1920", "1080")]
    [InlineData("2560x1440", "2560", "1440")]
    [InlineData("3840x2160", "3840", "2160")]
    public void ProcessLauncher_ResolutionPresets_EmitsExpectedVariables(string mode, string? expectedWidth, string? expectedHeight)
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var logger = new MockLoggerService();
        var launcher = new ProcessLauncher(env, validator, logger);

        var settings = new LauncherSettings();
        settings.Game.Resolution.Mode = mode;

        var config = launcher.BuildLaunchConfiguration(settings);

        if (expectedWidth == null)
        {
            Assert.False(config.EnvironmentVariables.ContainsKey("DC2_RESOLUTION_WIDTH"));
            Assert.False(config.EnvironmentVariables.ContainsKey("DC2_RESOLUTION_HEIGHT"));
        }
        else
        {
            Assert.True(config.EnvironmentVariables.ContainsKey("DC2_RESOLUTION_WIDTH"));
            Assert.Equal(expectedWidth, config.EnvironmentVariables["DC2_RESOLUTION_WIDTH"]);

            Assert.True(config.EnvironmentVariables.ContainsKey("DC2_RESOLUTION_HEIGHT"));
            Assert.Equal(expectedHeight, config.EnvironmentVariables["DC2_RESOLUTION_HEIGHT"]);
        }
    }

    [Fact]
    public void ProcessLauncher_CustomResolution_EmitsCustomWidthAndHeight()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var logger = new MockLoggerService();
        var launcher = new ProcessLauncher(env, validator, logger);

        var settings = new LauncherSettings();
        settings.Game.Resolution.Mode = "Custom";
        settings.Game.Resolution.Width = 3440;
        settings.Game.Resolution.Height = 1440;

        var config = launcher.BuildLaunchConfiguration(settings);

        Assert.True(config.EnvironmentVariables.ContainsKey("DC2_RESOLUTION_WIDTH"));
        Assert.Equal("3440", config.EnvironmentVariables["DC2_RESOLUTION_WIDTH"]);

        Assert.True(config.EnvironmentVariables.ContainsKey("DC2_RESOLUTION_HEIGHT"));
        Assert.Equal("1440", config.EnvironmentVariables["DC2_RESOLUTION_HEIGHT"]);
    }

    [Fact]
    public void ProcessLauncher_Fullscreen_EmitsOrOmitsVariable()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);
        var logger = new MockLoggerService();
        var launcher = new ProcessLauncher(env, validator, logger);

        var settings = new LauncherSettings();

        settings.Game.Resolution.Fullscreen = false;
        var configDisabled = launcher.BuildLaunchConfiguration(settings);
        Assert.False(configDisabled.EnvironmentVariables.ContainsKey("DC2_FULLSCREEN"));

        settings.Game.Resolution.Fullscreen = true;
        var configEnabled = launcher.BuildLaunchConfiguration(settings);
        Assert.True(configEnabled.EnvironmentVariables.ContainsKey("DC2_FULLSCREEN"));
        Assert.Equal("1", configEnabled.EnvironmentVariables["DC2_FULLSCREEN"]);
    }

    [Theory]
    [InlineData(1920, 1080, true)]
    [InlineData(-100, 1080, false)]
    [InlineData(1920, 0, false)]
    public void ValidationService_CustomResolution_ValidatesWidthAndHeight(int width, int height, bool expectedValid)
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncher" };
        var fs = new MockFileSystemService();
        var validator = new ValidationService(env, fs);

        var settings = new LauncherSettings();
        settings.Game.Resolution.Mode = "Custom";
        settings.Game.Resolution.Width = width;
        settings.Game.Resolution.Height = height;

        var result = validator.ValidateSettings(settings);

        Assert.Equal(expectedValid, result.IsValid);
    }

    [Fact]
    public void MainViewModel_CustomResolutionInputs_UpdatesViewModelAndSettings()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(tempDir);

        try
        {
            var env = new TestLauncherEnvironment { LauncherRoot = tempDir };
            var fs = new MockFileSystemService();
            var logger = new MockLoggerService();
            var settingsService = new JsonSettingsService(env, logger);
            var validationService = new ValidationService(env, fs);
            var processLauncher = new ProcessLauncher(env, validationService, logger);
            var isoExtractor = new MockIsoExtractor();

            var vm = new MainViewModel(env, settingsService, fs, processLauncher, validationService, isoExtractor);

            vm.SelectedResolutionMode = "Custom";
            Assert.True(vm.IsCustomResolutionSelected);

            vm.CustomWidth = "2560";
            vm.CustomHeight = "1080";
            vm.Fullscreen = true;
            vm.SkipIntro = true;

            Assert.Equal("2560", vm.CustomWidth);
            Assert.Equal("1080", vm.CustomHeight);
            Assert.True(vm.Fullscreen);
            Assert.True(vm.SkipIntro);

            var reloaded = settingsService.LoadSettings();
            Assert.Equal("Custom", reloaded.Game.Resolution.Mode);
            Assert.Equal(2560, reloaded.Game.Resolution.Width);
            Assert.Equal(1080, reloaded.Game.Resolution.Height);
            Assert.True(reloaded.Game.Resolution.Fullscreen);
            Assert.True(reloaded.Game.SkipIntro);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }
}
