"""EQ layer.

Pipeline::

    FeatureVector ─▶ Tag Score ─▶ EQ Preset ─▶ Final EQ

Layered imports
---------------
- :class:`FinalEQ`, :class:`EQPreset`, :class:`PresetStore`, and the mixer
  math are pure-logic — they need only :mod:`yaml` at import time.
- :class:`EQEngine` orchestrates the full pipeline and pulls in
  :mod:`features` (librosa) via :mod:`tagging`.  The import is wrapped in a
  ``try/except`` so the rest of this package stays available on a minimal
  install (useful for unit tests of the mixer).

Public entry points::

    from eq import EQEngine, FinalEQ
    result = EQEngine().compute(feature_vector)
    result.final_eq.hz_62               # → float, gain at 62 Hz
    result.final_eq.as_dict()           # → {31: ..., 62: ..., ..., 16000: ...}
    result.final_eq.to_json("out.json")

Live-reload::

    engine = EQEngine(auto_reload_presets=True)
    # Edit config/eq_presets.yaml → next compute() picks it up automatically.
"""
from __future__ import annotations

# Pure-logic — always available.
from eq.mixer import EQContribution, FinalEQ, MixerConfig, TagEQMixer
from eq.presets import (
    BAND_FREQUENCIES_HZ,
    EQBand,
    EQPreset,
    PresetStore,
    is_valid_band,
)

# Audio-dependent — requires the full features/tagging stack.  Guarded so
# `from eq import FinalEQ` still works on a minimal install.
try:
    from eq.engine import EQEngine, EQResult

    _AUDIO_STACK_AVAILABLE: bool = True
except ImportError:  # pragma: no cover — depends on install
    _AUDIO_STACK_AVAILABLE = False


__all__ = [
    # Constants
    "BAND_FREQUENCIES_HZ",
    "is_valid_band",
    # Preset dataclasses (always available)
    "EQBand",
    "EQPreset",
    "PresetStore",
    # Mixer output (always available)
    "FinalEQ",
    "EQContribution",
    "MixerConfig",
    "TagEQMixer",
    # Top-level engine (requires audio stack)
    "EQEngine",
    "EQResult",
]
