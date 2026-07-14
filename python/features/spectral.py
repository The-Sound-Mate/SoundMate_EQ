"""Spectrum features — FFT, STFT, and standard spectral descriptors.

All descriptors are per-frame arrays computed from a single STFT so the
different curves stay time-aligned.  ``fft_magnitude`` is the time-averaged
magnitude spectrum (= long-term average spectrum, LTAS), which is what most
callers mean when they say "the FFT of the whole track".
"""
from __future__ import annotations

from dataclasses import dataclass

import librosa
import numpy as np
from numpy.typing import NDArray


@dataclass(frozen=True, slots=True)
class SpectrumFeatures:
    """Frequency-domain descriptors.

    Shape convention:
        - ``stft_magnitude``:      ``(n_bins, n_frames)`` where ``n_bins = n_fft//2 + 1``.
        - ``fft_magnitude``:       ``(n_bins,)`` — mean magnitude across time (LTAS).
        - Per-frame descriptors:   ``(n_frames,)``.
        - ``spectral_contrast``:   ``(n_bands, n_frames)`` — one row per octave band.

    ``_mean`` variants are the time-averaged scalars intended for tagging /
    JSON summaries where the full array is too heavy.
    """

    # Raw transforms -----------------------------------------------------------
    fft_magnitude: NDArray[np.float32]        # (n_bins,)   time-averaged
    stft_magnitude: NDArray[np.float32]       # (n_bins, n_frames)

    # Per-frame descriptors ----------------------------------------------------
    spectral_centroid: NDArray[np.float32]    # (n_frames,) Hz
    spectral_bandwidth: NDArray[np.float32]   # (n_frames,) Hz
    spectral_contrast: NDArray[np.float32]    # (n_bands, n_frames)
    spectral_flatness: NDArray[np.float32]    # (n_frames,) 0..1
    spectral_rolloff: NDArray[np.float32]     # (n_frames,) Hz

    # Scalar summaries ---------------------------------------------------------
    spectral_centroid_mean: float
    spectral_bandwidth_mean: float
    spectral_contrast_mean: NDArray[np.float32]   # (n_bands,)
    spectral_flatness_mean: float
    spectral_rolloff_mean: float

    # STFT metadata (needed to reconstruct time / frequency axes) --------------
    n_fft: int
    hop_length: int


class SpectrumExtractor:
    """Compute STFT + standard spectral descriptors in one pass.

    All descriptors are derived from a single magnitude spectrogram ``S`` so
    they are time-aligned frame-by-frame.

    Args:
        n_fft: FFT size (also window size).
        hop_length: Hop between frames in samples.
        rolloff_percent: Fraction of total energy below the rolloff frequency.
    """

    def __init__(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        rolloff_percent: float = 0.85,
    ) -> None:
        self._n_fft = n_fft
        self._hop_length = hop_length
        self._rolloff_percent = rolloff_percent

    def extract(self, y: NDArray[np.floating], sr: int) -> SpectrumFeatures:
        """Extract spectrum features from a 1D mono signal."""
        # ---- Single STFT reused by all descriptors ---------------------------
        stft = librosa.stft(y, n_fft=self._n_fft, hop_length=self._hop_length)
        S = np.abs(stft).astype(np.float32)    # magnitude spectrogram
        ltas = S.mean(axis=1).astype(np.float32)  # long-term average spectrum

        # ---- Descriptors, all fed the pre-computed magnitude ----------------
        centroid = librosa.feature.spectral_centroid(S=S, sr=sr).flatten().astype(np.float32)
        bandwidth = librosa.feature.spectral_bandwidth(S=S, sr=sr).flatten().astype(np.float32)
        contrast = librosa.feature.spectral_contrast(S=S, sr=sr).astype(np.float32)
        flatness = librosa.feature.spectral_flatness(S=S).flatten().astype(np.float32)
        rolloff = librosa.feature.spectral_rolloff(
            S=S, sr=sr, roll_percent=self._rolloff_percent
        ).flatten().astype(np.float32)

        return SpectrumFeatures(
            fft_magnitude=ltas,
            stft_magnitude=S,
            spectral_centroid=centroid,
            spectral_bandwidth=bandwidth,
            spectral_contrast=contrast,
            spectral_flatness=flatness,
            spectral_rolloff=rolloff,
            spectral_centroid_mean=float(centroid.mean()) if centroid.size else 0.0,
            spectral_bandwidth_mean=float(bandwidth.mean()) if bandwidth.size else 0.0,
            spectral_contrast_mean=contrast.mean(axis=1).astype(np.float32),
            spectral_flatness_mean=float(flatness.mean()) if flatness.size else 0.0,
            spectral_rolloff_mean=float(rolloff.mean()) if rolloff.size else 0.0,
            n_fft=self._n_fft,
            hop_length=self._hop_length,
        )
