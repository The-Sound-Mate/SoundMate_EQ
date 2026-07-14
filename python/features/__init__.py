"""Feature extraction package.

Public entry point:
    ``FeatureExtractor(path) -> FeatureVector``
where :class:`FeatureVector` is a JSON-serializable aggregate of every
sub-domain's dataclass (:class:`TempoFeatures`, :class:`PitchFeatures`,
:class:`SpectrumFeatures`, :class:`LoudnessFeatures`, :class:`HarmonyFeatures`,
:class:`TimbreFeatures`, :class:`StereoFeatures`, :class:`RhythmFeatures`,
:class:`HPSSFeatures`).
"""
from __future__ import annotations

from features.extractor import FeatureExtractor, FeatureVector
from features.harmony import HarmonyExtractor, HarmonyFeatures
from features.hpss import HPSSExtractor, HPSSFeatures
from features.loudness import LoudnessExtractor, LoudnessFeatures
from features.pitch import PitchExtractor, PitchFeatures
from features.rhythm import RhythmExtractor, RhythmFeatures
from features.spectral import SpectrumExtractor, SpectrumFeatures
from features.stereo import StereoExtractor, StereoFeatures
from features.tempo import TempoExtractor, TempoFeatures
from features.timbre import TimbreExtractor, TimbreFeatures

__all__ = [
    # Top-level
    "FeatureExtractor",
    "FeatureVector",
    # Domain dataclasses
    "HarmonyFeatures",
    "HPSSFeatures",
    "LoudnessFeatures",
    "PitchFeatures",
    "RhythmFeatures",
    "SpectrumFeatures",
    "StereoFeatures",
    "TempoFeatures",
    "TimbreFeatures",
    # Domain extractors (rarely used directly; FeatureExtractor drives them)
    "HarmonyExtractor",
    "HPSSExtractor",
    "LoudnessExtractor",
    "PitchExtractor",
    "RhythmExtractor",
    "SpectrumExtractor",
    "StereoExtractor",
    "TempoExtractor",
    "TimbreExtractor",
]
