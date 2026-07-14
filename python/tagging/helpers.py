"""Intensity mapping helpers.

Rules translate raw features (a Hz value, a ratio, a dB level) into a
``[0.0, 1.0]`` confidence.  These small functions are the primitives every
rule composes:

    * ``ramp_up`` / ``ramp_down`` — one-sided linear ramps.
    * ``bell``                    — two-sided linear tent centered on a value.
    * ``gaussian_bell``           — smooth Gaussian tent.
    * ``sigmoid``                 — smooth S-curve.
    * ``combine_*``               — combine multiple partial intensities.
    * ``clip01``                  — final safety clamp.

None of them raise; they return ``0.0`` for degenerate inputs so a bad
threshold in YAML can never crash the pipeline.
"""
from __future__ import annotations

import math


def clip01(x: float) -> float:
    """Clamp to ``[0.0, 1.0]``."""
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    return x


def ramp_up(x: float, low: float, high: float) -> float:
    """Linear ramp: 0 at ``x <= low``, 1 at ``x >= high``.

    Use for "more is better" conditions such as *bright* (higher centroid → more).
    """
    if high <= low:
        # Degenerate config — treat as a step at ``low``.
        return 0.0 if x < low else 1.0
    if x <= low:
        return 0.0
    if x >= high:
        return 1.0
    return (x - low) / (high - low)


def ramp_down(x: float, low: float, high: float) -> float:
    """Linear ramp: 1 at ``x <= low``, 0 at ``x >= high`` (mirror of ``ramp_up``).

    Use for "less is better" conditions such as *dark* (lower centroid → more).
    """
    return 1.0 - ramp_up(x, low, high)


def bell(x: float, center: float, half_width: float) -> float:
    """Linear tent: 1 at ``x == center``, 0 at ``x == center ± half_width``.

    Use for "just right" conditions such as *bass_balanced*.
    """
    if half_width <= 0.0:
        return 1.0 if x == center else 0.0
    d = abs(x - center)
    if d >= half_width:
        return 0.0
    return 1.0 - d / half_width


def gaussian_bell(x: float, center: float, sigma: float) -> float:
    """Gaussian tent: smooth version of :func:`bell`.

    Never actually reaches 0 — useful when you want a soft "still relevant"
    tail rather than a hard cutoff.
    """
    if sigma <= 0.0:
        return 1.0 if x == center else 0.0
    return math.exp(-((x - center) ** 2) / (2.0 * sigma * sigma))


def sigmoid(x: float, midpoint: float, steepness: float = 1.0) -> float:
    """Smooth S-curve: 0.5 at ``x == midpoint``, saturates at 0 and 1.

    ``steepness`` controls how quickly the transition happens.  1.0 is very
    gradual; 10.0 is nearly a step.
    """
    z = -steepness * (x - midpoint)
    # Guard against overflow for very large negative x.
    if z > 500:
        return 0.0
    if z < -500:
        return 1.0
    return 1.0 / (1.0 + math.exp(z))


# -----------------------------------------------------------------------------
#  Combinators — glue multiple partial intensities into one.
# -----------------------------------------------------------------------------

def combine_geometric(*values: float) -> float:
    """Geometric mean of intensities → all must be non-zero for the result to be.

    Better than simple product because it stays in ``[0, 1]`` regardless of
    how many factors are combined.  Semantics: "*all* conditions should hold".
    """
    if not values:
        return 0.0
    prod = 1.0
    for v in values:
        prod *= max(0.0, v)
    return prod ** (1.0 / len(values))


def combine_min(*values: float) -> float:
    """Weakest link — the smallest partial intensity wins.

    Even stricter than :func:`combine_geometric`; use when a single missing
    condition should completely veto the tag.
    """
    if not values:
        return 0.0
    return max(0.0, min(values))


def combine_max(*values: float) -> float:
    """Strongest link — any single partial being high is enough.

    Semantics: "*any* of these conditions is enough to fire the tag".
    """
    if not values:
        return 0.0
    return min(1.0, max(values))


def combine_mean(*values: float) -> float:
    """Arithmetic mean — balanced contribution from every input."""
    if not values:
        return 0.0
    return clip01(sum(max(0.0, v) for v in values) / len(values))
