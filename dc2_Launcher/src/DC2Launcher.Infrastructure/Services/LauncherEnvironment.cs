using System;
using System.IO;
using DC2Launcher.Core.Interfaces;

namespace DC2Launcher.Infrastructure.Services;

public class LauncherEnvironment : ILauncherEnvironment
{
    public string LauncherRoot => AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

    public string ResolvePath(string relativeOrAbsolutePath)
    {
        if (string.IsNullOrWhiteSpace(relativeOrAbsolutePath))
            return LauncherRoot;

        if (Path.IsPathRooted(relativeOrAbsolutePath))
            return Path.GetFullPath(relativeOrAbsolutePath);

        return Path.GetFullPath(Path.Combine(LauncherRoot, relativeOrAbsolutePath));
    }
}
