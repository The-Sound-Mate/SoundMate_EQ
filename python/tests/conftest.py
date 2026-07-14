"""Shared pytest fixtures + audio-lib gating.

Some tests exercise pure logic (helpers, presets, mixer arithmetic) and run
anywhere.  Others exercise the audio pipeline (features, tagging.derived,
tagging.generator) and need librosa / pyloudnorm at import time.  We gate
the second group with the ``requires_audio_libs`` marker so ``pytest`` still
passes on a bare install.
"""
from __future__ import annotations

import importlib.util

import pytest


def _has(module: str) -> bool:
    return importlib.util.find_spec(module) is not None


HAS_LIBROSA = _has("librosa")
HAS_PYLOUDNORM = _has("pyloudnorm")
HAS_AUDIO_STACK = HAS_LIBROSA and HAS_PYLOUDNORM


requires_audio_libs = pytest.mark.skipif(
    not HAS_AUDIO_STACK,
    reason="requires librosa + pyloudnorm (`uv sync` in project root)",
)
