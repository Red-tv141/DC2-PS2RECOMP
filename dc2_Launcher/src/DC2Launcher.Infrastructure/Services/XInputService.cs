using System;
using System.Runtime.InteropServices;

namespace DC2Launcher.Infrastructure.Services;

public class XInputDetectedBinding
{
    public string KeyOrButton { get; set; } = string.Empty;
    public string DisplayName { get; set; } = string.Empty;
}

public static class XInputService
{
    [StructLayout(LayoutKind.Sequential)]
    public struct XINPUT_GAMEPAD
    {
        public ushort wButtons;
        public byte bLeftTrigger;
        public byte bRightTrigger;
        public short sThumbLX;
        public short sThumbLY;
        public short sThumbRX;
        public short sThumbRY;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct XINPUT_STATE
    {
        public uint dwPacketNumber;
        public XINPUT_GAMEPAD Gamepad;
    }

    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
    private static extern int XInputGetState14(int dwUserIndex, ref XINPUT_STATE pState);

    [DllImport("xinput1_3.dll", EntryPoint = "XInputGetState")]
    private static extern int XInputGetState13(int dwUserIndex, ref XINPUT_STATE pState);

    [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")]
    private static extern int XInputGetState910(int dwUserIndex, ref XINPUT_STATE pState);

    private static int _workingDll = 0; // 0 = untried, 1 = 1_4, 2 = 1_3, 3 = 9_1_0, -1 = unavailable

    public static bool GetControllerState(int userIndex, out XINPUT_STATE state)
    {
        state = default;
        if (_workingDll == -1) return false;

        if (_workingDll == 0 || _workingDll == 1)
        {
            try
            {
                if (XInputGetState14(userIndex, ref state) == 0)
                {
                    _workingDll = 1;
                    return true;
                }
            }
            catch { }
        }

        if (_workingDll == 0 || _workingDll == 2)
        {
            try
            {
                if (XInputGetState13(userIndex, ref state) == 0)
                {
                    _workingDll = 2;
                    return true;
                }
            }
            catch { }
        }

        if (_workingDll == 0 || _workingDll == 3)
        {
            try
            {
                if (XInputGetState910(userIndex, ref state) == 0)
                {
                    _workingDll = 3;
                    return true;
                }
            }
            catch { }
        }

        _workingDll = -1;
        return false;
    }

    public static XInputDetectedBinding? PollFirstPressedInput(int userIndex = 0)
    {
        if (!GetControllerState(userIndex, out var state)) return null;

        var gp = state.Gamepad;

        // Buttons
        if ((gp.wButtons & 0x1000) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_A", DisplayName = "Xbox Button A" };
        if ((gp.wButtons & 0x2000) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_B", DisplayName = "Xbox Button B" };
        if ((gp.wButtons & 0x4000) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_X", DisplayName = "Xbox Button X" };
        if ((gp.wButtons & 0x8000) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_Y", DisplayName = "Xbox Button Y" };

        if ((gp.wButtons & 0x0100) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_LB", DisplayName = "Xbox Left Bumper (LB)" };
        if ((gp.wButtons & 0x0200) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_RB", DisplayName = "Xbox Right Bumper (RB)" };

        if ((gp.wButtons & 0x0010) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_Start", DisplayName = "Xbox Start" };
        if ((gp.wButtons & 0x0020) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_Back", DisplayName = "Xbox Back / Select" };

        if ((gp.wButtons & 0x0040) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_LS", DisplayName = "Xbox Left Stick Click (L3)" };
        if ((gp.wButtons & 0x0080) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_RS", DisplayName = "Xbox Right Stick Click (R3)" };

        if ((gp.wButtons & 0x0001) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_DpadUp", DisplayName = "Xbox D-Pad Up" };
        if ((gp.wButtons & 0x0002) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_DpadDown", DisplayName = "Xbox D-Pad Down" };
        if ((gp.wButtons & 0x0004) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_DpadLeft", DisplayName = "Xbox D-Pad Left" };
        if ((gp.wButtons & 0x0008) != 0) return new XInputDetectedBinding { KeyOrButton = "XInput_DpadRight", DisplayName = "Xbox D-Pad Right" };

        // Triggers
        if (gp.bLeftTrigger > 80) return new XInputDetectedBinding { KeyOrButton = "XInput_LT", DisplayName = "Xbox Left Trigger (LT)" };
        if (gp.bRightTrigger > 80) return new XInputDetectedBinding { KeyOrButton = "XInput_RT", DisplayName = "Xbox Right Trigger (RT)" };

        // Analog Sticks
        const short stickThreshold = 18000;
        if (gp.sThumbLX < -stickThreshold) return new XInputDetectedBinding { KeyOrButton = "XInput_LeftStick_Left", DisplayName = "Xbox Left Stick Left" };
        if (gp.sThumbLX > stickThreshold) return new XInputDetectedBinding { KeyOrButton = "XInput_LeftStick_Right", DisplayName = "Xbox Left Stick Right" };
        if (gp.sThumbLY > stickThreshold) return new XInputDetectedBinding { KeyOrButton = "XInput_LeftStick_Up", DisplayName = "Xbox Left Stick Up" };
        if (gp.sThumbLY < -stickThreshold) return new XInputDetectedBinding { KeyOrButton = "XInput_LeftStick_Down", DisplayName = "Xbox Left Stick Down" };

        if (gp.sThumbRX < -stickThreshold) return new XInputDetectedBinding { KeyOrButton = "XInput_RightStick_Left", DisplayName = "Xbox Right Stick Left" };
        if (gp.sThumbRX > stickThreshold) return new XInputDetectedBinding { KeyOrButton = "XInput_RightStick_Right", DisplayName = "Xbox Right Stick Right" };
        if (gp.sThumbRY > stickThreshold) return new XInputDetectedBinding { KeyOrButton = "XInput_RightStick_Up", DisplayName = "Xbox Right Stick Up" };
        if (gp.sThumbRY < -stickThreshold) return new XInputDetectedBinding { KeyOrButton = "XInput_RightStick_Down", DisplayName = "Xbox Right Stick Down" };

        return null;
    }
}
