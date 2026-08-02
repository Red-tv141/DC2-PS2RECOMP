namespace DC2Launcher.Core.Models;

public class LauncherSettings
{
    public int SchemaVersion { get; set; } = 1;
    public GameOptions Game { get; set; } = new();
    public PathSettings Paths { get; set; } = new();
    public ModSettings Mods { get; set; } = new();
}

public class GameOptions
{
    public bool SkipIntro { get; set; } = false;
    public bool Enable60Fps { get; set; } = false;
    public bool EnableDebugMenu { get; set; } = false;

    public ResolutionSettings Resolution { get; set; } = new();
}

public class ResolutionSettings
{
    public string Mode { get; set; } = "GameDefault";
    public int? Width { get; set; } = null;
    public int? Height { get; set; } = null;
    public bool Fullscreen { get; set; } = false;
}

public class PathSettings
{
    public string Iso { get; set; } = "Dark Cloud 2 (USA) (v2.00).iso";
    public string Data { get; set; } = "DATA";
    public string Runner { get; set; } = @"bin\dc2_runner.exe";
    public string Elf { get; set; } = @"DATA\SCUS_972.13";
    public string MemoryCard1 { get; set; } = @"Saves\MemoryCard1";
    public string MemoryCard2 { get; set; } = @"Saves\MemoryCard2";
    public string Mods { get; set; } = "Mods";
    public string ControllerConfig { get; set; } = @"Config\controller.json";
}

public class ModSettings
{
    public bool Enabled { get; set; } = false;
}
