"""Loudness features — RMS, Peak, Dynamic Range, and BS.1770 LUFS.

- **RMS**: overall root-mean-square level, in dB (relative to full scale).
- **Peak**: sample-peak in dBFS.
- **Dynamic range**: peak − RMS (crest factor), a rough dynamics indicator.
- **LUFS**: BS.1770 integrated loudness via pyloudnorm — the broadcast /
  streaming standard used by Spotify, YouTube, Apple Music.
- **RMS envelope**: per-frame RMS for shape / dynamics-over-time analysis.
"""
from __future__ import annotations

from dataclasses import dataclass

import librosa
import numpy as np
import pyloudnorm as pyln
from numpy.typing import NDArray

_EPS: float = 1e-12  # avoids log(0)


@dataclass(frozen=True, slots=True)
class LoudnessFeatures:
    """Loudness / dynamics descriptors.

    Attributes:
        rms_db: Overall RMS in dBFS.
        peak_dbfs: Sample-peak in dBFS.
        dynamic_range_db: ``peak_dbfs − rms_db`` (crest factor).
        lufs_integrated: BS.1770 integrated loudness in LUFS.
        rms_envelope: Per-frame RMS in dBFS, shape ``(n_frames,)``.
    """

    rms_db: float
    peak_dbfs: float
    dynamic_range_db: float
    lufs_integrated: float
    rms_envelope: NDArray[np.float32]


class LoudnessExtractor:
    """Compute standard loudness / dynamics descriptors.

    Accepts either mono ``(N,)`` or stereo ``(2, N)`` input.  Stereo is
    preferred for LUFS since BS.1770 defines channel weighting; mono works
    fine but is treated as a single channel.

    Args:
        frame_length: Analysis window for the RMS envelope.
        hop_length: Hop for the RMS envelope.
    """

    def __init__(self, frame_length: int = 2048, hop_length: int = 512) -> None:
        self._frame_length = frame_length
        self._hop_length = hop_length

    def extract(self, y: NDArray[np.floating], sr: int) -> LoudnessFeatures:
        """Extract loudness features from mono ``(N,)`` or stereo ``(2, N)``."""
        # ---- Peak (dBFS) ----------------------------------------------------
        peak = float(np.max(np.abs(y))) if y.size else 0.0
        peak_dbfs = 20.0 * np.log10(peak + _EPS)

        # ---- Overall RMS (dBFS) --------------------------------------------
        rms_value = float(np.sqrt(np.mean(np.square(y)))) if y.size else 0.0
        rms_db = 20.0 * np.log10(rms_value + _EPS)

        # ---- Dynamic range (crest factor) -----------------------------------
        dynamic_range = peak_dbfs - rms_db

        # ---- LUFS via pyloudnorm (BS.1770) ---------------------------------
        # pyloudnorm expects ``(samples,)`` for mono or ``(samples, channels)``
        # for multi-channel.  Our input for stereo is ``(channels, samples)``.
        if y.ndim == 1:
            lufs_input = y.astype(np.float64)
        else:
            lufs_input = y.T.astype(np.float64)

        try:
            meter = pyln.Meter(sr)
            lufs = float(meter.integrated_loudness(lufs_input))
            # pyloudnorm returns -inf for very quiet / silent input.
            if not np.isfinite(lufs):
                lufs = -np.inf
        except ValueError:
            # Signal shorter than 400 ms → pyloudnorm cannot compute LUFS.
            lufs = -np.inf

        # ---- Per-frame RMS envelope (mono) ---------------------------------
        y_mono = y if y.ndim == 1 else librosa.to_mono(y)
        rms_env = librosa.feature.rms(
            y=y_mono,
            frame_length=self._frame_length,
            hop_length=self._hop_length,
        ).flatten()
        rms_env_db = (20.0 * np.log10(rms_env + _EPS)).astype(np.float32)

        return LoudnessFeatures(
            rms_db=rms_db,
            peak_dbfs=peak_dbfs,
            dynamic_range_db=dynamic_range,
            lufs_integrated=lufs,
            rms_envelope=rms_env_db,
        )
