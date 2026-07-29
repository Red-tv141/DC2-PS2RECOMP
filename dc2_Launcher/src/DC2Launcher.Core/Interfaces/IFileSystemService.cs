namespace DC2Launcher.Core.Interfaces;

public interface IFileSystemService
{
    bool FileExists(string path);
    bool DirectoryExists(string path);
    long GetFileSize(string path);
    void CreateDirectory(string path);
    void OpenFolderInExplorer(string path);
}
