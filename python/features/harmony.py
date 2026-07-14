"""Harmony features — Chroma (12 pitch classes) and Tonnetz (6-D tonal space).

Chroma folds the spectrum into 12 pitch classes and is the standard input for
key / chord estimation.  Tonnetz projects chroma into a 6-D space where
fifth / third relationships become Euclidean distances — useful for detecting
harmonic movement.
"""
from __future__ import annotations

from dataclasses import dataclass

import librosa
import numpy as np
from numpy.typing import NDArray


@dataclass(frozen=True, slots=True)
class HarmonyFeatures:
    """Harmonic descriptors.

    Shapes:
        - ``chroma``:    ``(12, n_frames)`` — energy per pitch class over time.
        - ``tonnetz``:   ``(6,  n_frames)`` — perfect-fifth, minor-third,
                          major-third relationships (2 dims each).
        - ``_mean`` variants collapse the time axis to scalars / short vectors.
    """

    chroma: NDArray[np.float32]           # (12, n_frames)
    chroma_mean: NDArray[np.float32]      # (12,)
    tonnetz: NDArray[np.float32]          # (6, n_frames)
    tonnetz_mean: NDArray[np.float32]     # (6,)


class HarmonyExtractor:
    """Chroma + Tonnetz via librosa.

    Uses ``chroma_cqt`` when practical (better pitch invariance than ``chroma_stft``)
    but falls back gracefully if the signal is too short for the CQT.

    Args:
        n_fft: FFT size for the STFT-based chroma fallback.
        hop_length: Hop between frames in samples.
    """

    def __init__(self, n_fft: int = 2048, hop_length: int = 512) -> None:
        self._n_fft = n_fft
        self._hop_length = hop_length

    def extract(self, y: NDArray[np.floating], sr: int) -> HarmonyFeatures:
        """Extract chroma + tonnetz from a 1D mono signal.

        Tonnetz is only well-defined for harmonic content, so we run HPSS
        internally to isolate the harmonic component before projecting.
        librosa does the same in its own `tonnetz` example.
        """
        chroma = self._chroma(y, sr)

        # Tonnetz operates on the harmonic component to avoid smearing from
        # percussive transients.  ``librosa.effects.harmonic`` returns the
        # harmonic residual as a signal we can re-chroma.
        y_harmonic = librosa.effects.harmonic(y)
        tonnetz = librosa.feature.tonnetz(y=y_harmonic, sr=sr).astype(np.float32)

        return HarmonyFeatures(
            chroma=chroma,
            chroma_mean=chroma.mean(axis=1).astype(np.float32),
            tonnetz=tonnetz,
            tonnetz_mean=tonnetz.mean(axis=1).astype(np.float32),
        )

    # ------------------------------------------------------------------ private
    def _chroma(self, y: NDArray[np.floating], sr: int) -> NDArray[np.float32]:
        """Prefer CQT chroma; fall back to STFT chroma for short signals."""
        try:
            return librosa.feature.chroma_cqt(
                y=y, sr=sr, hop_length=self._hop_length
            ).astype(np.float32)
        except Exception:  # signal too short / librosa CQT edge cases
            return librosa.feature.chroma_stft(
                y=y, sr=sr, n_fft=self._n_fft, hop_length=self._hop_length
            ).astype(np.float32)
