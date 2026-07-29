using System;
using System.IO;
using DC2Launcher.Core.Interfaces;

namespace DC2Launcher.Infrastructure.Services;

public class FileLoggerService : ILoggerService
{
    private readonly string _logFilePath;
    private readonly object _lock = new();

    public string LogFilePath => _logFilePath;

    public FileLoggerService(ILauncherEnvironment environment)
    {
        var logsDir = Path.Combine(environment.LauncherRoot, "Logs");
        Directory.CreateDirectory(logsDir);
        _logFilePath = Path.Combine(logsDir, "launcher.log");
    }

    public void LogInfo(string message) => WriteLog("INFO", message);
    public void LogWarning(string message) => WriteLog("WARN", message);
    public void LogError(string message, Exception? ex = null)
    {
        var fullMessage = ex != null ? $"{message} | Exception: {ex.Message}" : message;
        WriteLog("ERROR", fullMessage);
    }

    private void WriteLog(string level, string message)
    {
        lock (_lock)
        {
            try
            {
                var entry = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] [{level}] {message}{Environment.NewLine}";
                File.AppendAllText(_logFilePath, entry);
            }
            catch
            {
                // Prevent logging failure from crashing launcher
            }
        }
    }
}
