namespace DC2Launcher.Core.Interfaces;

public interface ILauncherEnvironment
{
    string LauncherRoot { get; }
    string ResolvePath(string relativeOrAbsolutePath);
}
