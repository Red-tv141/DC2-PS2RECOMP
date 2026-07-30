using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Media;
using System.Windows.Media;
using DC2Launcher.Core.Services;

namespace DC2Launcher.App.Services;

/// <summary>
/// WPF implementation of IAudioService using System.Windows.Media.MediaPlayer and SoundPlayer.
/// </summary>
public class WpfAudioService : IAudioService
{
    private MediaPlayer? _mediaPlayer;
    private bool _isStopped;
    private readonly Dictionary<string, SoundPlayer> _sfxPlayers = new(StringComparer.OrdinalIgnoreCase);

    public void PlayLooping(string filePath)
    {
        if (string.IsNullOrWhiteSpace(filePath))
        {
            return;
        }

        var fullPath = Path.GetFullPath(filePath);
        if (!File.Exists(fullPath))
        {
            Debug.WriteLine($"[WpfAudioService] File not found: {fullPath}");
            return;
        }

        try
        {
            Stop();

            _isStopped = false;
            _mediaPlayer = new MediaPlayer();
            _mediaPlayer.Volume = 1.0;

            _mediaPlayer.MediaFailed += (s, e) =>
            {
                Debug.WriteLine($"[WpfAudioService] Media failed to play: {e.ErrorException?.Message}");
            };

            _mediaPlayer.MediaEnded += (s, e) =>
            {
                if (!_isStopped && _mediaPlayer != null)
                {
                    _mediaPlayer.Position = TimeSpan.Zero;
                    _mediaPlayer.Play();
                }
            };

            _mediaPlayer.Open(new Uri(fullPath, UriKind.Absolute));
            _mediaPlayer.Play();
            Debug.WriteLine($"[WpfAudioService] Started looping audio: {fullPath}");
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[WpfAudioService] Exception starting audio: {ex.Message}");
        }
    }

    public void Stop()
    {
        _isStopped = true;
        if (_mediaPlayer != null)
        {
            try
            {
                _mediaPlayer.Stop();
                _mediaPlayer.Close();
                Debug.WriteLine("[WpfAudioService] Stopped audio playback.");
            }
            catch
            {
                // Ignore cleanup failures
            }
            finally
            {
                _mediaPlayer = null;
            }
        }
    }

    public void PlaySfx(string sfxFileName)
    {
        if (string.IsNullOrWhiteSpace(sfxFileName)) return;

        try
        {
            var candidates = new[]
            {
                Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "SFX", sfxFileName),
                Path.Combine(AppContext.BaseDirectory, "SFX", sfxFileName),
                Path.Combine("SFX", sfxFileName),
                sfxFileName
            };

            string? foundPath = null;
            foreach (var candidate in candidates)
            {
                if (File.Exists(candidate))
                {
                    foundPath = Path.GetFullPath(candidate);
                    break;
                }
            }

            if (foundPath != null)
            {
                if (!_sfxPlayers.TryGetValue(foundPath, out var player))
                {
                    player = new SoundPlayer(foundPath);
                    player.LoadAsync();
                    _sfxPlayers[foundPath] = player;
                }
                player.Play();
            }
        }
        catch
        {
            // Gracefully ignore SFX playback errors
        }
    }
}
