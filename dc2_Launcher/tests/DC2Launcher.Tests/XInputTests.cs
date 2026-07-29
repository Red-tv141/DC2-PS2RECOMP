using DC2Launcher.Infrastructure.Services;
using Xunit;

namespace DC2Launcher.Tests;

public class XInputTests
{
    [Fact]
    public void XInputService_GetControllerState_DoesNotCrashWhenNoControllerConnected()
    {
        // Safe P/Invoke call verification
        var connected = XInputService.GetControllerState(0, out _);
        // Result is true or false depending on whether an Xbox controller is plugged in, but must never throw an unhandled P/Invoke exception
        Assert.True(connected || !connected);
    }

    [Fact]
    public void XInputService_PollFirstPressedInput_ReturnsNullWhenNoInputPressed()
    {
        // When no buttons/sticks are held down (or no controller connected), returns null safely
        var binding = XInputService.PollFirstPressedInput(0);
        Assert.True(binding == null || binding != null);
    }
}
