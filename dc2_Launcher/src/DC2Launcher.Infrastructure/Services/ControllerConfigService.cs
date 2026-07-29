using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;

namespace DC2Launcher.Infrastructure.Services;

public class ControllerConfigService : IControllerConfigService
{
    private readonly ILoggerService _logger;
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    public ControllerConfigService(ILoggerService logger)
    {
        _logger = logger ?? throw new ArgumentNullException(nameof(logger));
    }

    public ControllerConfig GetDefaultConfig()
    {
        var config = new ControllerConfig
        {
            SchemaVersion = 1,
            LeftStickDeadZonePercent = 10.0,
            RightStickDeadZonePercent = 10.0
        };

        void Map(Ps2Button btn, string device, string key, string name)
        {
            config.Mappings[btn.ToString()] = new InputBinding
            {
                DeviceType = device,
                KeyOrButton = key,
                DisplayName = name
            };
        }

        Map(Ps2Button.DpadUp, "Keyboard", "Up", "Up Arrow");
        Map(Ps2Button.DpadDown, "Keyboard", "Down", "Down Arrow");
        Map(Ps2Button.DpadLeft, "Keyboard", "Left", "Left Arrow");
        Map(Ps2Button.DpadRight, "Keyboard", "Right", "Right Arrow");

        Map(Ps2Button.LeftStickUp, "Keyboard", "W", "Key W");
        Map(Ps2Button.LeftStickDown, "Keyboard", "S", "Key S");
        Map(Ps2Button.LeftStickLeft, "Keyboard", "A", "Key A");
        Map(Ps2Button.LeftStickRight, "Keyboard", "D", "Key D");
        Map(Ps2Button.L3, "Keyboard", "LeftShift", "Left Shift");

        Map(Ps2Button.RightStickUp, "Keyboard", "NumPad8", "Numpad 8");
        Map(Ps2Button.RightStickDown, "Keyboard", "NumPad2", "Numpad 2");
        Map(Ps2Button.RightStickLeft, "Keyboard", "NumPad4", "Numpad 4");
        Map(Ps2Button.RightStickRight, "Keyboard", "NumPad6", "Numpad 6");
        Map(Ps2Button.R3, "Keyboard", "RightShift", "Right Shift");

        Map(Ps2Button.Cross, "Keyboard", "Space", "Space Bar (X)");
        Map(Ps2Button.Circle, "Keyboard", "E", "Key E (Circle)");
        Map(Ps2Button.Square, "Keyboard", "R", "Key R (Square)");
        Map(Ps2Button.Triangle, "Keyboard", "F", "Key F (Triangle)");

        Map(Ps2Button.L1, "Keyboard", "Q", "Key Q (L1)");
        Map(Ps2Button.L2, "Keyboard", "D1", "Key 1 (L2)");
        Map(Ps2Button.R1, "Keyboard", "C", "Key C (R1)");
        Map(Ps2Button.R2, "Keyboard", "D2", "Key 2 (R2)");

        Map(Ps2Button.Start, "Keyboard", "Return", "Enter Key (Start)");
        Map(Ps2Button.Select, "Keyboard", "Back", "Backspace Key (Select)");

        return config;
    }

    public ControllerConfig LoadConfig(string configFilePath)
    {
        if (string.IsNullOrWhiteSpace(configFilePath) || !File.Exists(configFilePath))
        {
            _logger.LogInfo($"Controller config '{configFilePath}' not found. Initializing defaults.");
            return GetDefaultConfig();
        }

        try
        {
            var json = File.ReadAllText(configFilePath);
            var config = JsonSerializer.Deserialize<ControllerConfig>(json, JsonOptions);
            if (config != null)
            {
                // Ensure all 24 PS2 controls are mapped
                var defaults = GetDefaultConfig();
                foreach (var kvp in defaults.Mappings)
                {
                    if (!config.Mappings.ContainsKey(kvp.Key))
                    {
                        config.Mappings[kvp.Key] = kvp.Value;
                    }
                }
                return config;
            }
        }
        catch (Exception ex)
        {
            _logger.LogError($"Failed to load controller config '{configFilePath}'. Recovering defaults.", ex);
        }

        return GetDefaultConfig();
    }

    public void SaveConfig(string configFilePath, ControllerConfig config)
    {
        if (string.IsNullOrWhiteSpace(configFilePath)) throw new ArgumentNullException(nameof(configFilePath));
        if (config == null) throw new ArgumentNullException(nameof(config));

        try
        {
            var dir = Path.GetDirectoryName(configFilePath);
            if (!string.IsNullOrWhiteSpace(dir) && !Directory.Exists(dir))
            {
                Directory.CreateDirectory(dir);
            }

            var json = JsonSerializer.Serialize(config, JsonOptions);
            var tempPath = configFilePath + ".tmp";

            File.WriteAllText(tempPath, json);

            if (File.Exists(configFilePath))
            {
                File.Replace(tempPath, configFilePath, null);
            }
            else
            {
                File.Move(tempPath, configFilePath);
            }

            _logger.LogInfo($"Controller config saved successfully to '{configFilePath}'.");
        }
        catch (Exception ex)
        {
            _logger.LogError($"Failed to save controller config to '{configFilePath}'", ex);
        }
    }

    public List<string> ValidateConfig(ControllerConfig config)
    {
        var issues = new List<string>();
        if (config == null)
        {
            issues.Add("Controller config is null.");
            return issues;
        }

        if (config.LeftStickDeadZonePercent < 0 || config.LeftStickDeadZonePercent > 50)
        {
            issues.Add("Left Stick Dead Zone must be between 0% and 50%.");
        }

        if (config.RightStickDeadZonePercent < 0 || config.RightStickDeadZonePercent > 50)
        {
            issues.Add("Right Stick Dead Zone must be between 0% and 50%.");
        }

        // Duplicate Binding Check
        var grouped = config.Mappings
            .Where(m => !string.IsNullOrWhiteSpace(m.Value.KeyOrButton))
            .GroupBy(m => $"{m.Value.DeviceType}:{m.Value.KeyOrButton}", StringComparer.OrdinalIgnoreCase)
            .Where(g => g.Count() > 1);

        foreach (var group in grouped)
        {
            var controls = string.Join(", ", group.Select(g => g.Key));
            issues.Add($"Duplicate Binding: Key/Button '{group.Key}' is mapped to multiple controls ({controls}).");
        }

        return issues;
    }
}
