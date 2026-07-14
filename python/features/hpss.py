"""HPSS features — Harmonic vs Percussive energy split.

librosa's ``effects.hpss`` factors a signal into a sustained-tonal component
(harmonic) and a transient component (percussive) by median filtering the
STFT.  Comparing their energies tells us how "drum-heavy" a track is.
"""
from __future__ import annotations

from dataclasses import dataclass

import librosa
import numpy as np
from numpy.typing import NDArray

_EPS: float = 1e-12


@dataclass(frozen=True, slots=True)
class HPSSFeatures:
    """Harmonic / Percussive decomposition summary.

    Attributes:
        harmonic_energy: RMS of the harmonic component (linear scale, ≥ 0).
        percussive_energy: RMS of the percussive component (linear scale, ≥ 0).
        harmonic_ratio: ``H / (H + P)`` in ``[0, 1]``.
                        1.0 = purely harmonic, 0.0 = purely percussive.
        percussive_ratio: ``P / (H + P)`` in ``[0, 1]``.  Sums to 1 with
                          ``harmonic_ratio`` by construction.
        harmonic_energy_db: Harmonic RMS expressed in dBFS.
        percussive_energy_db: Percussive RMS expressed in dBFS.
    """

    harmonic_energy: float
    percussive_energy: float
    harmonic_ratio: float
    percussive_ratio: float
    harmonic_energy_db: float
    percussive_energy_db: float


class HPSSExtractor:
    """Split a mono signal into harmonic / percussive parts and summarize energy.

    Args:
        margin: Optional HPSS margin parameter (see ``librosa.effects.hpss``).
                Higher values produce a "harder" separation.
    """

    def __init__(self, margin: float = 1.0) -> None:
        self._margin = margin

    def extract(self, y: NDArray[np.floating], sr: int) -> HPSSFeatures:  # noqa: ARG002
        """Extract HPSS energy features from a 1D mono signal.

        The ``sr`` argument is accepted for a uniform Extractor signature but
        is not needed — HPSS operates entirely on the STFT.
        """
        # ``librosa.effects.hpss`` returns two time-domain signals.
        y_harmonic, y_percussive = librosa.effects.hpss(y, margin=self._margin)

        # Per-component RMS.  Kept in linear scale (energy proxy).
        h_rms = float(np.sqrt(np.mean(np.square(y_harmonic))))
        p_rms = float(np.sqrt(np.mean(np.square(y_percussive))))

        total = h_rms + p_rms
        if total < _EPS:
            # Silent input → ratios are undefined; return 0.5/0.5 by convention.
            h_ratio = 0.5
            p_ratio = 0.5
        else:
            h_ratio = h_rms / total
            p_ratio = p_rms / total

        return HPSSFeatures(
            harmonic_energy=h_rms,
            percussive_energy=p_rms,
            harmonic_ratio=h_ratio,
            percussive_ratio=p_ratio,
            harmonic_energy_db=float(20.0 * np.log10(h_rms + _EPS)),
            percussive_energy_db=float(20.0 * np.log10(p_rms + _EPS)),
        )
