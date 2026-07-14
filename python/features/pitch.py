"""Pitch features — fundamental frequency (f0) and pitch-class histogram.

Uses ``librosa.pyin`` (probabilistic YIN) for f0 tracking.  A 12-bin histogram
over voiced frames' pitch classes summarizes the tonal content of the track.
"""
from __future__ import annotations

from dataclasses import dataclass

import librosa
import numpy as np
from numpy.typing import NDArray


@dataclass(frozen=True, slots=True)
class PitchFeatures:
    """Tonal descriptors.

    Attributes:
        f0_hz: Per-frame fundamental frequency in Hz.  ``NaN`` for unvoiced
            frames (this is how librosa.pyin reports them). Shape ``(n_frames,)``.
        f0_median_hz: Median f0 over voiced frames (0.0 if none detected).
        voiced_ratio: Fraction of frames flagged as voiced, in ``[0, 1]``.
        pitch_histogram: Normalized 12-bin histogram over pitch classes
            (C, C#, D, ..., B), sums to 1.0. Shape ``(12,)``.
    """

    f0_hz: NDArray[np.float32]
    f0_median_hz: float
    voiced_ratio: float
    pitch_histogram: NDArray[np.float32]


class PitchExtractor:
    """f0 tracking via probabilistic YIN + 12-bin pitch-class histogram.

    Args:
        fmin_hz: Lower search bound (default C2 ≈ 65.4 Hz).
        fmax_hz: Upper search bound (default C7 ≈ 2093 Hz).
        frame_length: Analysis frame length in samples.
        hop_length: Hop between frames in samples.
    """

    def __init__(
        self,
        fmin_hz: float = 65.4,
        fmax_hz: float = 2093.0,
        frame_length: int = 2048,
        hop_length: int = 512,
    ) -> None:
        self._fmin = fmin_hz
        self._fmax = fmax_hz
        self._frame_length = frame_length
        self._hop_length = hop_length

    def extract(self, y: NDArray[np.floating], sr: int) -> PitchFeatures:
        """Extract pitch features from a 1D mono signal ``y`` at rate ``sr``."""
        # pYIN returns:
        #   f0            — Hz per frame (NaN where unvoiced)
        #   voiced_flag   — bool per frame
        #   voiced_prob   — probability per frame (unused here)
        f0, voiced_flag, _ = librosa.pyin(
            y=y,
            fmin=self._fmin,
            fmax=self._fmax,
            sr=sr,
            frame_length=self._frame_length,
            hop_length=self._hop_length,
        )

        f0 = np.asarray(f0, dtype=np.float32)
        voiced_flag = np.asarray(voiced_flag, dtype=bool)

        # ---- Aggregate statistics --------------------------------------------
        voiced_ratio = float(voiced_flag.mean()) if voiced_flag.size else 0.0
        voiced_f0 = f0[voiced_flag & np.isfinite(f0)]
        f0_median = float(np.median(voiced_f0)) if voiced_f0.size else 0.0

        # ---- 12-bin pitch class histogram ------------------------------------
        histogram = self._pitch_class_histogram(voiced_f0)

        return PitchFeatures(
            f0_hz=f0,
            f0_median_hz=f0_median,
            voiced_ratio=voiced_ratio,
            pitch_histogram=histogram,
        )

    @staticmethod
    def _pitch_class_histogram(voiced_f0_hz: NDArray[np.float32]) -> NDArray[np.float32]:
        """Convert voiced f0 values → 12-bin pitch-class histogram, normalized."""
        if voiced_f0_hz.size == 0:
            return np.zeros(12, dtype=np.float32)

        # Convert Hz → MIDI number → pitch class (C=0, C#=1, ..., B=11).
        midi = librosa.hz_to_midi(voiced_f0_hz)
        pc = np.rint(midi).astype(int) % 12

        counts = np.bincount(pc, minlength=12).astype(np.float32)
        total = counts.sum()
        if total == 0:
            return np.zeros(12, dtype=np.float32)
        return (counts / total).astype(np.float32)
