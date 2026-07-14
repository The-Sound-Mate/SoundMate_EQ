"""Stereo features — width and Left/Right balance.

Width uses the mid/side decomposition:  more side-energy relative to mid
means a wider image.  L/R balance compares per-channel RMS to detect
mono-panned or level-imbalanced mixes.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray

_EPS: float = 1e-12


@dataclass(frozen=True, slots=True)
class StereoFeatures:
    """Stereo image descriptors.

    All values are ``0.0`` for mono input.

    Attributes:
        stereo_width: ``RMS(side) / (RMS(mid) + RMS(side))`` in ``[0, 1]``.
                      0 = fully mono, 1 = fully side (uncorrelated L/R).
        lr_balance: ``(RMS(R) − RMS(L)) / (RMS(R) + RMS(L))`` in ``[-1, +1]``.
                    −1 = only left, 0 = balanced, +1 = only right.
        lr_correlation: Pearson correlation between L and R, in ``[-1, +1]``.
                        +1 = mono-like, 0 = uncorrelated, −1 = phase-inverted.
        is_stereo: True if input has 2 channels and L ≠ R (i.e. not fake stereo).
    """

    stereo_width: float
    lr_balance: float
    lr_correlation: float
    is_stereo: bool


class StereoExtractor:
    """Compute stereo width, L/R balance, and correlation from a stereo signal."""

    def extract(self, y: NDArray[np.floating], sr: int) -> StereoFeatures:  # noqa: ARG002
        """Extract stereo features from ``(N,)`` mono or ``(2, N)`` stereo.

        The ``sr`` argument is accepted for a uniform Extractor signature but
        is not used — all stereo metrics are dimensionless.
        """
        # ---- Mono short-circuit ---------------------------------------------
        if y.ndim == 1 or y.shape[0] == 1:
            return StereoFeatures(
                stereo_width=0.0,
                lr_balance=0.0,
                lr_correlation=1.0,   # perfectly mono → correlation 1
                is_stereo=False,
            )

        # ---- Split L / R (drop extra channels beyond the first two) --------
        L = y[0].astype(np.float64)
        R = y[1].astype(np.float64)

        # ---- Mid / Side decomposition → width -------------------------------
        mid = 0.5 * (L + R)
        side = 0.5 * (L - R)
        rms_mid = float(np.sqrt(np.mean(np.square(mid))))
        rms_side = float(np.sqrt(np.mean(np.square(side))))
        width = rms_side / (rms_mid + rms_side + _EPS)

        # ---- L / R balance --------------------------------------------------
        rms_l = float(np.sqrt(np.mean(np.square(L))))
        rms_r = float(np.sqrt(np.mean(np.square(R))))
        balance = (rms_r - rms_l) / (rms_r + rms_l + _EPS)

        # ---- Pearson correlation -------------------------------------------
        # If either channel is silent, correlation is undefined; report 0.
        if rms_l < _EPS or rms_r < _EPS:
            corr = 0.0
        else:
            corr = float(np.corrcoef(L, R)[0, 1])

        # Fake-stereo check: if L == R exactly, treat as mono.
        is_stereo = bool(not np.allclose(L, R))

        return StereoFeatures(
            stereo_width=width,
            lr_balance=balance,
            lr_correlation=corr,
            is_stereo=is_stereo,
        )
