using System.Collections.Generic;

namespace DC2Launcher.Core.Models;

public class LaunchConfiguration
{
    public string ExecutablePath { get; set; } = string.Empty;
    public string WorkingDirectory { get; set; } = string.Empty;
    public List<string> Arguments { get; set; } = new();
    public Dictionary<string, string> EnvironmentVariables { get; set; } = new();

    public string FormatDiagnosticSummary()
    {
        var sb = new System.Text.StringBuilder();
        sb.AppendLine($"Executable: {ExecutablePath}");
        sb.AppendLine($"Working Directory: {WorkingDirectory}");
        sb.AppendLine($"Arguments: {string.Join(" ", Arguments)}");
        sb.AppendLine("Environment Variables:");
        if (EnvironmentVariables.Count == 0)
        {
            sb.AppendLine("  (None set)");
        }
        else
        {
            foreach (var kvp in EnvironmentVariables)
            {
                sb.AppendLine($"  {kvp.Key} = {kvp.Value}");
            }
        }
        return sb.ToString().TrimEnd();
    }
}
