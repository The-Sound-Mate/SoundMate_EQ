"""Rhythm features — onset strength envelope and detected onset positions.

The onset strength envelope is a per-frame signal that peaks when new events
begin (drum hits, note attacks, etc.).  Detected onsets are the peak positions
in that envelope, in seconds.

Note: BPM and beat positions live in ``features.tempo`` — this module is only
about attack density, not metrical structure.
"""
from __future__ import annotations

from dataclasses import dataclass

import librosa
import numpy as np
from numpy.typing import NDArray


@dataclass(frozen=True, slots=True)
class RhythmFeatures:
    """Onset-related descriptors.

    Attributes:
        onset_strength: Per-frame novelty function, shape ``(n_frames,)``.
        onset_times_sec: Detected onset positions in seconds, shape ``(n_onsets,)``.
        onset_rate_per_sec: Onsets per second across the whole track.
        hop_length: Hop between frames — needed to reconstruct the time axis.
    """

    onset_strength: NDArray[np.float32]
    onset_times_sec: NDArray[np.float32]
    onset_rate_per_sec: float
    hop_length: int


class RhythmExtractor:
    """Onset novelty envelope + peak-picking via librosa.

    Args:
        hop_length: Hop between frames in samples.
    """

    def __init__(self, hop_length: int = 512) -> None:
        self._hop_length = hop_length

    def extract(self, y: NDArray[np.floating], sr: int) -> RhythmFeatures:
        """Extract onset features from a 1D mono signal."""
        # Onset strength = flux of the mel spectrogram — a standard novelty function.
        onset_env = librosa.onset.onset_strength(
            y=y, sr=sr, hop_length=self._hop_length
        ).astype(np.float32)

        # Peak-pick to get discrete onset positions (in seconds).
        onset_times = librosa.onset.onset_detect(
            onset_envelope=onset_env,
            sr=sr,
            hop_length=self._hop_length,
            units="time",
        ).astype(np.float32)

        # Duration derived from the source signal, not the envelope, so the
        # rate is meaningful even if `onset_env` is zero-padded at the tail.
        duration = float(len(y)) / float(sr) if sr > 0 else 0.0
        rate = float(onset_times.size) / duration if duration > 0 else 0.0

        return RhythmFeatures(
            onset_strength=onset_env,
            onset_times_sec=onset_times,
            onset_rate_per_sec=rate,
            hop_length=self._hop_length,
        )
