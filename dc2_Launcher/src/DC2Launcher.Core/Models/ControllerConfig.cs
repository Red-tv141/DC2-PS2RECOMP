using System;
using System.Collections.Generic;

namespace DC2Launcher.Core.Models;

public enum Ps2Button
{
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftStickUp,
    LeftStickDown,
    LeftStickLeft,
    LeftStickRight,
    L3,
    RightStickUp,
    RightStickDown,
    RightStickLeft,
    RightStickRight,
    R3,
    Cross,
    Circle,
    Square,
    Triangle,
    L1,
    L2,
    R1,
    R2,
    Start,
    Select
}

public class InputBinding
{
    public string DeviceType { get; set; } = "Keyboard";
    public string KeyOrButton { get; set; } = string.Empty;
    public string DisplayName { get; set; } = string.Empty;

    public override bool Equals(object? obj)
    {
        if (obj is InputBinding other)
        {
            return string.Equals(DeviceType, other.DeviceType, StringComparison.OrdinalIgnoreCase) &&
                   string.Equals(KeyOrButton, other.KeyOrButton, StringComparison.OrdinalIgnoreCase);
        }
        return false;
    }

    public override int GetHashCode()
    {
        return HashCode.Combine(DeviceType.ToLowerInvariant(), KeyOrButton.ToLowerInvariant());
    }
}

public class ControllerConfig
{
    public int SchemaVersion { get; set; } = 1;
    public double LeftStickDeadZonePercent { get; set; } = 10.0;
    public double RightStickDeadZonePercent { get; set; } = 10.0;
    public Dictionary<string, InputBinding> Mappings { get; set; } = new(StringComparer.OrdinalIgnoreCase);
}
