using System.Collections.Generic;

namespace DC2Launcher.Core.Models;

public class ValidationResult
{
    public bool IsValid => Errors.Count == 0;
    public List<string> Errors { get; } = new();
    public List<string> Warnings { get; } = new();

    public void AddError(string error)
    {
        if (!string.IsNullOrWhiteSpace(error))
        {
            Errors.Add(error);
        }
    }

    public void AddWarning(string warning)
    {
        if (!string.IsNullOrWhiteSpace(warning))
        {
            Warnings.Add(warning);
        }
    }

    public static ValidationResult Success() => new();

    public static ValidationResult Failure(string error)
    {
        var result = new ValidationResult();
        result.AddError(error);
        return result;
    }

    public static ValidationResult Failure(IEnumerable<string> errors)
    {
        var result = new ValidationResult();
        foreach (var err in errors)
        {
            result.AddError(err);
        }
        return result;
    }
}
