namespace DC2Launcher.Core.Services;

/// <summary>
/// Service interface for playing launcher audio and sound effects.
/// </summary>
public interface IAudioService
{
    /// <summary>
    /// Starts playing the specified audio file in a loop.
    /// </summary>
    void PlayLooping(string filePath);

    /// <summary>
    /// Stops playing the current audio track.
    /// </summary>
    void Stop();

    /// <summary>
    /// Plays a short UI sound effect (SFX).
    /// </summary>
    void PlaySfx(string sfxFileName);
}
