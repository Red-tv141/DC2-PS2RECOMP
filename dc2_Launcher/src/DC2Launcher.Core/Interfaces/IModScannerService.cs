using DC2Launcher.Core.Models;

namespace DC2Launcher.Core.Interfaces;

public interface IModScannerService
{
    ModScanResult ScanModsDirectory(string modsDirectoryPath);
    void SaveManifest(string manifestFilePath, ModScanResult scanResult);
}
