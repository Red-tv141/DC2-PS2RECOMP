using System;

namespace DC2Launcher.Core.Interfaces;

public interface ILoggerService
{
    string LogFilePath { get; }
    void LogInfo(string message);
    void LogWarning(string message);
    void LogError(string message, Exception? ex = null);
}
