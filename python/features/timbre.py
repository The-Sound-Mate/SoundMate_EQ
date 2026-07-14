"""Timbre features — MFCC (Mel-Frequency Cepstral Coefficients).

MFCC is the standard "timbre fingerprint" in MIR: it captures the shape of
the spectral envelope in a compact vector.  The first coefficient encodes
overall loudness and is often dropped in classification tasks; we keep it so
downstream callers can decide.
"""
from __future__ import annotations

from dataclasses import dataclass

import librosa
import numpy as np
from numpy.typing import NDArray


@dataclass(frozen=True, slots=True)
class TimbreFeatures:
    """Timbral descriptors.

    Attributes:
        mfcc: Per-frame MFCC matrix, shape ``(n_mfcc, n_frames)``.
        mfcc_mean: Time-averaged MFCC vector, shape ``(n_mfcc,)``.
        mfcc_std: Standard deviation of each MFCC over time, shape ``(n_mfcc,)``.
        mfcc_delta: First-order time derivative (velocity), shape ``(n_mfcc, n_frames)``.
        mfcc_delta_mean: Time-averaged delta, shape ``(n_mfcc,)``.
        n_mfcc: Number of MFCC coefficients used.
    """

    mfcc: NDArray[np.float32]
    mfcc_mean: NDArray[np.float32]
    mfcc_std: NDArray[np.float32]
    mfcc_delta: NDArray[np.float32]
    mfcc_delta_mean: NDArray[np.float32]
    n_mfcc: int


class TimbreExtractor:
    """Compute MFCC + delta MFCC via librosa.

    Args:
        n_mfcc: Number of coefficients (13 is the classic MIR default).
        n_fft: FFT / window size for the mel spectrogram.
        hop_length: Hop between frames in samples.
        n_mels: Number of mel filters.
    """

    def __init__(
        self,
        n_mfcc: int = 13,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mels: int = 128,
    ) -> None:
        self._n_mfcc = n_mfcc
        self._n_fft = n_fft
        self._hop_length = hop_length
        self._n_mels = n_mels

    def extract(self, y: NDArray[np.floating], sr: int) -> TimbreFeatures:
        """Extract MFCC + delta MFCC from a 1D mono signal."""
        # MFCC = DCT of log-mel spectrogram.
        mfcc = librosa.feature.mfcc(
            y=y,
            sr=sr,
            n_mfcc=self._n_mfcc,
            n_fft=self._n_fft,
            hop_length=self._hop_length,
            n_mels=self._n_mels,
        ).astype(np.float32)

        # First-order delta captures short-term timbre change ("attack" vs "sustain").
        mfcc_delta = librosa.feature.delta(mfcc).astype(np.float32)

        return TimbreFeatures(
            mfcc=mfcc,
            mfcc_mean=mfcc.mean(axis=1).astype(np.float32),
            mfcc_std=mfcc.std(axis=1).astype(np.float32),
            mfcc_delta=mfcc_delta,
            mfcc_delta_mean=mfcc_delta.mean(axis=1).astype(np.float32),
            n_mfcc=self._n_mfcc,
        )
