"""AudioLoader + AudioData — file-not-found and dataclass logic.

The synthetic-WAV roundtrip test requires soundfile / librosa and is skipped
on a bare install.
"""
from __future__ import annotations

import numpy as np
import pytest

from audio.loader import AudioData, AudioLoader, AudioLoadError
from tests.conftest import requires_audio_libs


# =============================================================================
#  AudioData — pure logic, no audio-lib dependency
# =============================================================================

class TestAudioData:
    def test_duration_mono(self):
        audio = AudioData(
            samples=np.zeros(44100, dtype=np.float32),
            samplerate=44100,
            channels=1,
        )
        assert audio.duration == pytest.approx(1.0)

    def test_duration_stereo(self):
        # Stereo shape is (channels, frames).
        audio = AudioData(
            samples=np.zeros((2, 22050), dtype=np.float32),
            samplerate=44100,
            channels=2,
        )
        assert audio.duration == pytest.approx(0.5)

    def test_num_frames_mono(self):
        audio = AudioData(
            samples=np.zeros(1024, dtype=np.float32),
            samplerate=44100, channels=1,
        )
        assert audio.num_frames == 1024

    def test_num_frames_stereo(self):
        audio = AudioData(
            samples=np.zeros((2, 4096), dtype=np.float32),
            samplerate=44100, channels=2,
        )
        assert audio.num_frames == 4096

    def test_is_mono(self):
        mono = AudioData(np.zeros(1), samplerate=44100, channels=1)
        stereo = AudioData(np.zeros((2, 1)), samplerate=44100, channels=2)
        assert mono.is_mono
        assert not stereo.is_mono

    def test_zero_samplerate_no_zero_division(self):
        audio = AudioData(np.zeros(100), samplerate=0, channels=1)
        # Guard against 1/0 blowing up in duration.
        assert audio.duration == 0.0

    def test_frozen(self):
        audio = AudioData(np.zeros(100), samplerate=44100, channels=1)
        with pytest.raises(Exception):  # FrozenInstanceError
            audio.samplerate = 22050  # type: ignore[misc]


# =============================================================================
#  AudioLoader — error paths (no audio libs needed for missing-file case)
# =============================================================================

class TestAudioLoaderErrors:
    def test_missing_file_raises_file_not_found(self, tmp_path):
        loader = AudioLoader()
        with pytest.raises(FileNotFoundError):
            loader.load(tmp_path / "does_not_exist.wav")

    def test_directory_raises_file_not_found(self, tmp_path):
        loader = AudioLoader()
        # Passing a dir path (which exists but isn't a file) is a common mistake.
        with pytest.raises(FileNotFoundError):
            loader.load(tmp_path)

    def test_corrupt_file_raises_audio_load_error(self, tmp_path):
        # A text file with a .wav extension is not decodable.
        bad = tmp_path / "not-really.wav"
        bad.write_text("this is not audio")
        loader = AudioLoader()
        with pytest.raises(AudioLoadError):
            loader.load(bad)


# =============================================================================
#  AudioLoader — happy path (needs soundfile + librosa)
# =============================================================================

@requires_audio_libs
class TestAudioLoaderHappyPath:
    def test_wav_roundtrip(self, tmp_path):
        import soundfile as sf

        # Synthesize a 1-second 440 Hz sine at 22050 Hz mono.
        sr = 22050
        duration = 1.0
        t = np.linspace(0, duration, int(sr * duration), endpoint=False)
        signal = (0.5 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)

        wav_path = tmp_path / "sine.wav"
        sf.write(wav_path, signal, sr)

        audio = AudioLoader(target_sr=sr).load(wav_path)
        assert audio.samplerate == sr
        assert audio.duration == pytest.approx(1.0, abs=0.02)
        assert audio.channels == 1
        assert audio.samples.dtype == np.float32
        assert audio.source_path == wav_path

    def test_resample_on_load(self, tmp_path):
        import soundfile as sf

        # Write at 44.1 kHz, request 22.05 kHz.
        sr_in, sr_target = 44100, 22050
        t = np.linspace(0, 0.5, int(sr_in * 0.5), endpoint=False)
        signal = (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)
        wav_path = tmp_path / "resample.wav"
        sf.write(wav_path, signal, sr_in)

        audio = AudioLoader(target_sr=sr_target).load(wav_path)
        assert audio.samplerate == sr_target
        assert audio.duration == pytest.approx(0.5, abs=0.02)

    def test_stereo_preserved(self, tmp_path):
        import soundfile as sf

        sr = 22050
        t = np.linspace(0, 0.25, int(sr * 0.25), endpoint=False)
        left = (0.4 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)
        right = (0.4 * np.sin(2 * np.pi * 330 * t)).astype(np.float32)
        stereo = np.stack([left, right], axis=1)  # soundfile wants (frames, channels)
        wav_path = tmp_path / "stereo.wav"
        sf.write(wav_path, stereo, sr)

        audio = AudioLoader(target_sr=sr, mono=False).load(wav_path)
        assert audio.channels == 2
        # librosa returns (channels, frames) for multi-channel.
        assert audio.samples.shape[0] == 2

    def test_to_mono_downmix(self, tmp_path):
        import soundfile as sf

        sr = 22050
        t = np.linspace(0, 0.1, int(sr * 0.1), endpoint=False)
        left = (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)
        right = (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)
        stereo = np.stack([left, right], axis=1)
        wav_path = tmp_path / "twoch.wav"
        sf.write(wav_path, stereo, sr)

        audio = AudioLoader(target_sr=sr, mono=False).load(wav_path)
        mono = audio.to_mono()
        assert mono.is_mono
        assert mono.samples.ndim == 1
        assert mono.channels == 1
