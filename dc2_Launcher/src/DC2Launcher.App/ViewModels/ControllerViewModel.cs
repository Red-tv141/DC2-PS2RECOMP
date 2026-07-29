using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Windows.Input;
using System.Windows.Threading;
using DC2Launcher.Core.Interfaces;
using DC2Launcher.Core.Models;
using DC2Launcher.Infrastructure.Services;
using InputBinding = DC2Launcher.Core.Models.InputBinding;

namespace DC2Launcher.App.ViewModels;

public class ControllerViewModel : ViewModelBase
{
    private readonly IControllerConfigService _configService;
    private readonly ILauncherEnvironment _environment;
    private readonly string _configFilePath;
    private readonly DispatcherTimer _captureTimer;

    private ControllerConfig _config;
    private bool _isCapturingInput;
    private string _capturingButtonName = string.Empty;
    private int _captureTimeoutSeconds = 5;
    private int _timerTicksCount = 0;
    private ButtonBindingItemViewModel? _capturingItem;
    private string _validationStatusMessage = string.Empty;

    public ControllerViewModel(
        ILauncherEnvironment environment,
        IControllerConfigService? configService = null,
        Action? closeAction = null)
    {
        _environment = environment ?? throw new ArgumentNullException(nameof(environment));
        _configService = configService ?? new ControllerConfigService(new FileLoggerService(environment));
        CloseAction = closeAction;

        _configFilePath = Path.Combine(_environment.LauncherRoot, "Config", "controller.json");
        _config = _configService.LoadConfig(_configFilePath);

        ButtonBindings = new ObservableCollection<ButtonBindingItemViewModel>();
        DuplicateWarnings = new ObservableCollection<string>();

        _captureTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(50) // 50ms tick interval for responsive XInput controller polling
        };
        _captureTimer.Tick += OnCaptureTimerTick;

        ResetDefaultsCommand = new RelayCommand(ResetDefaults);
        SaveAndCloseCommand = new RelayCommand(SaveAndClose);
        CancelCommand = new RelayCommand(Cancel);
        CancelRebindCommand = new RelayCommand(CancelRebind);

        InitializeBindings();
        ValidateConfiguration();
    }

    public Action? CloseAction { get; set; }
    public ObservableCollection<ButtonBindingItemViewModel> ButtonBindings { get; }
    public ObservableCollection<string> DuplicateWarnings { get; }

    public double LeftStickDeadZone
    {
        get => _config.LeftStickDeadZonePercent;
        set
        {
            if (_config.LeftStickDeadZonePercent != value)
            {
                _config.LeftStickDeadZonePercent = Math.Clamp(value, 0.0, 50.0);
                OnPropertyChanged();
                ValidateConfiguration();
            }
        }
    }

    public double RightStickDeadZone
    {
        get => _config.RightStickDeadZonePercent;
        set
        {
            if (_config.RightStickDeadZonePercent != value)
            {
                _config.RightStickDeadZonePercent = Math.Clamp(value, 0.0, 50.0);
                OnPropertyChanged();
                ValidateConfiguration();
            }
        }
    }

    public bool IsCapturingInput
    {
        get => _isCapturingInput;
        set => SetProperty(ref _isCapturingInput, value);
    }

    public string CapturingButtonName
    {
        get => _capturingButtonName;
        set => SetProperty(ref _capturingButtonName, value);
    }

    public int CaptureTimeoutSeconds
    {
        get => _captureTimeoutSeconds;
        set => SetProperty(ref _captureTimeoutSeconds, value);
    }

    public string ValidationStatusMessage
    {
        get => _validationStatusMessage;
        set => SetProperty(ref _validationStatusMessage, value);
    }

    public ICommand ResetDefaultsCommand { get; }
    public ICommand SaveAndCloseCommand { get; }
    public ICommand CancelCommand { get; }
    public ICommand CancelRebindCommand { get; }

    public void HandleKeyDown(Key key)
    {
        if (!IsCapturingInput || _capturingItem == null) return;

        if (key == Key.Escape)
        {
            CancelRebind();
            return;
        }

        var keyName = key.ToString();
        var displayName = $"Key {keyName}";

        _config.Mappings[_capturingItem.ButtonName] = new InputBinding
        {
            DeviceType = "Keyboard",
            KeyOrButton = keyName,
            DisplayName = displayName
        };

        _capturingItem.DeviceType = "Keyboard";
        _capturingItem.BoundKey = displayName;

        StopCapture();
        ValidateConfiguration();
    }

    private void StartRebind(ButtonBindingItemViewModel item)
    {
        _capturingItem = item;
        CapturingButtonName = item.DisplayName;
        CaptureTimeoutSeconds = 5;
        _timerTicksCount = 0;
        IsCapturingInput = true;
        _captureTimer.Start();
    }

    private void CancelRebind()
    {
        StopCapture();
    }

    private void StopCapture()
    {
        _captureTimer.Stop();
        IsCapturingInput = false;
        _capturingItem = null;
    }

    private void OnCaptureTimerTick(object? sender, EventArgs e)
    {
        if (!IsCapturingInput || _capturingItem == null)
        {
            StopCapture();
            return;
        }

        // Poll Xbox / XInput controller for button, trigger, or stick input
        try
        {
            var xinputBinding = XInputService.PollFirstPressedInput(0);
            if (xinputBinding != null)
            {
                _config.Mappings[_capturingItem.ButtonName] = new InputBinding
                {
                    DeviceType = "XInput",
                    KeyOrButton = xinputBinding.KeyOrButton,
                    DisplayName = xinputBinding.DisplayName
                };

                _capturingItem.DeviceType = "XInput";
                _capturingItem.BoundKey = xinputBinding.DisplayName;

                StopCapture();
                ValidateConfiguration();
                return;
            }
        }
        catch { }

        // Decrement timeout counter every 20 ticks (50ms * 20 = 1000ms = 1 second)
        _timerTicksCount++;
        if (_timerTicksCount % 20 == 0)
        {
            CaptureTimeoutSeconds--;
            if (CaptureTimeoutSeconds <= 0)
            {
                StopCapture();
            }
        }
    }

    private void InitializeBindings()
    {
        ButtonBindings.Clear();
        var defaults = _configService.GetDefaultConfig();

        foreach (Ps2Button btn in Enum.GetValues(typeof(Ps2Button)))
        {
            var btnName = btn.ToString();
            if (!_config.Mappings.TryGetValue(btnName, out var binding))
            {
                binding = defaults.Mappings[btnName];
                _config.Mappings[btnName] = binding;
            }

            var displayName = FormatPs2ButtonDisplayName(btn);
            ButtonBindings.Add(new ButtonBindingItemViewModel(btn, displayName, binding, StartRebind));
        }
    }

    private string FormatPs2ButtonDisplayName(Ps2Button btn) => btn switch
    {
        Ps2Button.DpadUp => "D-Pad Up",
        Ps2Button.DpadDown => "D-Pad Down",
        Ps2Button.DpadLeft => "D-Pad Left",
        Ps2Button.DpadRight => "D-Pad Right",
        Ps2Button.LeftStickUp => "Left Stick Up",
        Ps2Button.LeftStickDown => "Left Stick Down",
        Ps2Button.LeftStickLeft => "Left Stick Left",
        Ps2Button.LeftStickRight => "Left Stick Right",
        Ps2Button.L3 => "L3 (Left Stick Click)",
        Ps2Button.RightStickUp => "Right Stick Up",
        Ps2Button.RightStickDown => "Right Stick Down",
        Ps2Button.RightStickLeft => "Right Stick Left",
        Ps2Button.RightStickRight => "Right Stick Right",
        Ps2Button.R3 => "R3 (Right Stick Click)",
        Ps2Button.Cross => "Cross (A / Bottom Action)",
        Ps2Button.Circle => "Circle (B / Right Action)",
        Ps2Button.Square => "Square (X / Left Action)",
        Ps2Button.Triangle => "Triangle (Y / Top Action)",
        Ps2Button.L1 => "L1 (Left Bumper)",
        Ps2Button.L2 => "L2 (Left Trigger)",
        Ps2Button.R1 => "R1 (Right Bumper)",
        Ps2Button.R2 => "R2 (Right Trigger)",
        Ps2Button.Start => "Start",
        Ps2Button.Select => "Select / Back",
        _ => btn.ToString()
    };

    private void ValidateConfiguration()
    {
        DuplicateWarnings.Clear();

        var duplicates = _configService.ValidateConfig(_config);
        foreach (var dup in duplicates)
        {
            DuplicateWarnings.Add(dup);
        }

        if (DuplicateWarnings.Count > 0)
        {
            ValidationStatusMessage = $"Warning: {DuplicateWarnings.Count} duplicate key/button binding(s) detected.";
        }
        else
        {
            ValidationStatusMessage = "Controller configuration valid — No key binding conflicts.";
        }
    }

    private void ResetDefaults()
    {
        _config = _configService.GetDefaultConfig();
        InitializeBindings();
        ValidateConfiguration();
    }

    private void SaveAndClose()
    {
        _configService.SaveConfig(_configFilePath, _config);
        CloseAction?.Invoke();
    }

    private void Cancel()
    {
        CloseAction?.Invoke();
    }
}

public class ButtonBindingItemViewModel : ViewModelBase
{
    private string _deviceType;
    private string _boundKey;

    public ButtonBindingItemViewModel(
        Ps2Button button,
        string displayName,
        InputBinding binding,
        Action<ButtonBindingItemViewModel> startRebindAction)
    {
        Ps2Button = button;
        ButtonName = button.ToString();
        DisplayName = displayName;
        _deviceType = binding.DeviceType;
        _boundKey = binding.DisplayName;
        RebindCommand = new RelayCommand(() => startRebindAction(this));
    }

    public Ps2Button Ps2Button { get; }
    public string ButtonName { get; }
    public string DisplayName { get; }

    public string DeviceType
    {
        get => _deviceType;
        set => SetProperty(ref _deviceType, value);
    }

    public string BoundKey
    {
        get => _boundKey;
        set => SetProperty(ref _boundKey, value);
    }

    public ICommand RebindCommand { get; }
}
