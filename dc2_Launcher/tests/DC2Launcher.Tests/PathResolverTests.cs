using System.IO;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class TestLauncherEnvironment : ILauncherEnvironment
{
    public string LauncherRoot { get; set; } = @"C:\MockLauncherDir";

    public string ResolvePath(string relativeOrAbsolutePath)
    {
        if (string.IsNullOrWhiteSpace(relativeOrAbsolutePath))
            return LauncherRoot;

        if (Path.IsPathRooted(relativeOrAbsolutePath))
            return Path.GetFullPath(relativeOrAbsolutePath);

        return Path.GetFullPath(Path.Combine(LauncherRoot, relativeOrAbsolutePath));
    }
}

public class PathResolverTests
{
    [Fact]
    public void Resolve_RelativePath_ReturnsAbsolutePathUnderLauncherRoot()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncherDir" };
        var resolver = new PathResolver(env);

        var result = resolver.Resolve(@"bin\dc2_runner.exe");

        Assert.Equal(Path.GetFullPath(@"C:\MockLauncherDir\bin\dc2_runner.exe"), result);
    }

    [Fact]
    public void Resolve_AbsolutePath_ReturnsUnchangedAbsolutePath()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncherDir" };
        var resolver = new PathResolver(env);

        var result = resolver.Resolve(@"D:\ExternalSaves\Card1");

        Assert.Equal(Path.GetFullPath(@"D:\ExternalSaves\Card1"), result);
    }

    [Fact]
    public void Resolve_PathWithSpaces_ResolvesCorrectly()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\Game Folder With Spaces" };
        var resolver = new PathResolver(env);

        var result = resolver.Resolve(@"Dark Cloud 2 (USA) (v2.00).iso");

        Assert.Equal(Path.GetFullPath(@"C:\Game Folder With Spaces\Dark Cloud 2 (USA) (v2.00).iso"), result);
    }

    [Fact]
    public void MakeRelativeIfUnderRoot_ChildPath_ReturnsRelativePath()
    {
        var env = new TestLauncherEnvironment { LauncherRoot = @"C:\MockLauncherDir" };
        var resolver = new PathResolver(env);

        var result = resolver.MakeRelativeIfUnderRoot(@"C:\MockLauncherDir\Saves\MemoryCard1");

        Assert.Equal(@"Saves\MemoryCard1", result);
    }
}
