using System;
using System.Windows;
using DC2Launcher.Infrastructure.Services;

namespace DC2Launcher.App;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        DispatcherUnhandledException += (sender, args) =>
        {
            LogAndShowException(args.Exception, "UI Thread Unhandled Exception");
            args.Handled = true;
        };

        AppDomain.CurrentDomain.UnhandledException += (sender, args) =>
        {
            if (args.ExceptionObject is Exception ex)
            {
                LogAndShowException(ex, "Domain Unhandled Exception");
            }
        };
    }

    private static void LogAndShowException(Exception ex, string context)
    {
        try
        {
            var env = new LauncherEnvironment();
            var logger = new FileLoggerService(env);
            logger.LogError($"[Hardening] {context}: {ex.Message}", ex);
        }
        catch
        {
            // Ignore logging failure to prevent recursive crash
        }

        MessageBox.Show(
            $"An unexpected error occurred ({context}):\n\n{ex.Message}\n\nRecommended Action: Check Logs/launcher.log for details.",
            "Dark Cloud 2 Launcher — Unexpected Error",
            MessageBoxButton.OK,
            MessageBoxImage.Error);
    }
}
