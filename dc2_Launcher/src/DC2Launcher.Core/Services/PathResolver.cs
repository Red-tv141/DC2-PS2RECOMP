using System;
using System.IO;
using DC2Launcher.Core.Interfaces;

namespace DC2Launcher.Core.Services;

public class PathResolver
{
    private readonly ILauncherEnvironment _environment;

    public PathResolver(ILauncherEnvironment environment)
    {
        _environment = environment ?? throw new ArgumentNullException(nameof(environment));
    }

    public string Resolve(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return _environment.LauncherRoot;

        if (Path.IsPathRooted(path))
            return Path.GetFullPath(path);

        return Path.GetFullPath(Path.Combine(_environment.LauncherRoot, path));
    }

    public string MakeRelativeIfUnderRoot(string absolutePath)
    {
        if (string.IsNullOrWhiteSpace(absolutePath))
            return absolutePath;

        if (!Path.IsPathRooted(absolutePath))
            return absolutePath;

        var root = _environment.LauncherRoot;
        if (!root.EndsWith(Path.DirectorySeparatorChar.ToString()))
            root += Path.DirectorySeparatorChar;

        var fullPath = Path.GetFullPath(absolutePath);
        if (fullPath.StartsWith(root, StringComparison.OrdinalIgnoreCase))
        {
            return fullPath.Substring(root.Length);
        }

        return absolutePath;
    }
}
