using System;
using System.IO;
using DC2Launcher.Core.Interfaces;

namespace DC2Launcher.Core.Services;

public static class ModOverrideResolver
{
    public static string ResolveEffectiveAssetPath(
        string relativeAssetPath,
        string modsRootPath,
        string dataRootPath,
        bool modsEnabled,
        IFileSystemService fileSystem)
    {
        if (string.IsNullOrWhiteSpace(relativeAssetPath)) throw new ArgumentNullException(nameof(relativeAssetPath));
        if (fileSystem == null) throw new ArgumentNullException(nameof(fileSystem));

        var normalizedRelative = relativeAssetPath.Replace('/', '\\').TrimStart('\\');

        if (modsEnabled && !string.IsNullOrWhiteSpace(modsRootPath))
        {
            var candidateModPath = Path.Combine(modsRootPath, normalizedRelative);
            if (fileSystem.FileExists(candidateModPath))
            {
                return candidateModPath;
            }
        }

        return Path.Combine(dataRootPath, normalizedRelative);
    }
}
