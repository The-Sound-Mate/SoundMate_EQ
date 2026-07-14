"""FeatureVector aggregate + FeatureExtractor orchestrator + JSON I/O.

Pipeline
--------
    AudioData  ──▶  ┌────────────────────────────────────────────────────────┐
                    │  Tempo   Pitch    Spectrum  Timbre   Loudness          │
                    │  Harmony Stereo   Rhythm    HPSS                       │
                    │  (each Extractor is an isolated, replaceable class)    │
                    └────────────────────────────────────────────────────────┘
                                        │
                                        ▼
                                 FeatureVector
                                (frozen dataclass)
                                        │
                                        ▼
                            to_dict() / to_json(path)
                            from_dict() / from_json(path)

Design notes
------------
- ``FeatureVector`` is a **frozen** dataclass.  Feature values are numeric
  ndarrays / floats and never mutated after construction.
- :meth:`FeatureExtractor.extract` takes an :class:`AudioData` (produced by
  :class:`audio.AudioLoader`) — file loading is a *separate* pipeline stage,
  which makes the extractor testable with synthetic buffers.
- JSON is produced by walking the dataclass tree once, converting numpy
  types to native Python.  ``from_dict`` rebuilds the tree explicitly so the
  ndarray dtypes stay consistent across a save/load round trip.
- Large arrays (STFT, MFCC, chroma) roundtrip fine but produce sizable JSON.
  For compact summaries, pass ``include_arrays=False`` to ``to_dict`` /
  ``to_json`` — only scalar summary stats survive.
"""
from __future__ import annotations

import dataclasses
import json
import logging
from dataclasses import dataclass, fields
from pathlib import Path
from typing import Any

import librosa
import numpy as np
from numpy.typing import NDArray

from audio.loader import AudioData
from features.harmony import HarmonyExtractor, HarmonyFeatures
from features.hpss import HPSSExtractor, HPSSFeatures
from features.loudness import LoudnessExtractor, LoudnessFeatures
from features.pitch import PitchExtractor, PitchFeatures
from features.rhythm import RhythmExtractor, RhythmFeatures
from features.spectral import SpectrumExtractor, SpectrumFeatures
from features.stereo import StereoExtractor, StereoFeatures
from features.tempo import TempoExtractor, TempoFeatures
from features.timbre import TimbreExtractor, TimbreFeatures

logger = logging.getLogger(__name__)


# =============================================================================
#  FeatureVector — top-level container returned to callers
# =============================================================================

# Names of fields on each sub-dataclass whose value is a "large" ndarray.
# When ``include_arrays=False``, these are dropped from the serialized dict.
_LARGE_ARRAY_FIELDS: dict[str, tuple[str, ...]] = {
    "tempo":    (),
    "pitch":    ("f0_hz",),
    "spectrum": ("stft_magnitude", "spectral_centroid", "spectral_bandwidth",
                 "spectral_contrast", "spectral_flatness", "spectral_rolloff",
                 "fft_magnitude"),
    "timbre":   ("mfcc", "mfcc_delta"),
    "loudness": ("rms_envelope",),
    "harmony":  ("chroma", "tonnetz"),
    "stereo":   (),
    "rhythm":   ("onset_strength",),
    "hpss":     (),
}


@dataclass(frozen=True, slots=True)
class FeatureVector:
    """Aggregated, JSON-serializable feature representation of an audio file.

    Attributes:
        source_path: Original file path (stringified for JSON portability).
        samplerate: Sample rate used for analysis (Hz).
        duration_sec: Length of the analyzed signal in seconds.
        channels: Channel count of the original file (1 = mono, 2 = stereo, ...).
        tempo / pitch / spectrum / loudness / harmony / timbre / stereo /
        rhythm / hpss: per-domain feature dataclasses.
    """

    # -- metadata -------------------------------------------------------------
    source_path: str
    samplerate: int
    duration_sec: float
    channels: int

    # -- feature groups (in the same order as the user's spec) ---------------
    tempo: TempoFeatures
    pitch: PitchFeatures
    spectrum: SpectrumFeatures
    loudness: LoudnessFeatures
    harmony: HarmonyFeatures
    timbre: TimbreFeatures
    stereo: StereoFeatures
    rhythm: RhythmFeatures
    hpss: HPSSFeatures

    # ------------------------------------------------------------------ export
    def to_dict(self, include_arrays: bool = True) -> dict[str, Any]:
        """Convert to a JSON-safe nested dict.

        Args:
            include_arrays: If True, per-frame arrays (STFT, MFCC, chroma,
                RMS envelope, ...) are included.  If False, only scalar
                summaries survive — useful for compact summaries.
        """
        result: dict[str, Any] = {
            "source_path": self.source_path,
            "samplerate": int(self.samplerate),
            "duration_sec": float(self.duration_sec),
            "channels": int(self.channels),
        }
        for group_field in fields(self):
            if group_field.name in ("source_path", "samplerate", "duration_sec", "channels"):
                continue
            group_obj = getattr(self, group_field.name)
            skip = () if include_arrays else _LARGE_ARRAY_FIELDS.get(group_field.name, ())
            result[group_field.name] = _dataclass_to_dict(group_obj, skip_fields=skip)
        return result

    def to_json(
        self,
        path: str | Path,
        *,
        indent: int | None = 2,
        include_arrays: bool = True,
    ) -> Path:
        """Serialize to a JSON file.  Returns the written path."""
        out_path = Path(path)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        data = self.to_dict(include_arrays=include_arrays)
        with out_path.open("w", encoding="utf-8") as f:
            json.dump(data, f, indent=indent, ensure_ascii=False)
        return out_path

    # ------------------------------------------------------------------ import
    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "FeatureVector":
        """Reconstruct a FeatureVector from a dict produced by ``to_dict``.

        Missing per-frame arrays (e.g. if the source was saved with
        ``include_arrays=False``) are restored as empty ndarrays so the
        shape contract is preserved.
        """
        return cls(
            source_path=str(data["source_path"]),
            samplerate=int(data["samplerate"]),
            duration_sec=float(data["duration_sec"]),
            channels=int(data["channels"]),
            tempo=_build_tempo(data["tempo"]),
            pitch=_build_pitch(data["pitch"]),
            spectrum=_build_spectrum(data["spectrum"]),
            loudness=_build_loudness(data["loudness"]),
            harmony=_build_harmony(data["harmony"]),
            timbre=_build_timbre(data["timbre"]),
            stereo=_build_stereo(data["stereo"]),
            rhythm=_build_rhythm(data["rhythm"]),
            hpss=_build_hpss(data["hpss"]),
        )

    @classmethod
    def from_json(cls, path: str | Path) -> "FeatureVector":
        """Load a FeatureVector from a JSON file previously written by ``to_json``."""
        with Path(path).open("r", encoding="utf-8") as f:
            data = json.load(f)
        return cls.from_dict(data)


# =============================================================================
#  FeatureExtractor — the pipeline the user drives
# =============================================================================

class FeatureExtractor:
    """Turn :class:`AudioData` into a :class:`FeatureVector`.

    Instantiate once and reuse — every sub-extractor is stateless.
    Loading is intentionally *not* this class's job; use
    :class:`audio.AudioLoader` upstream to keep stages independently
    testable.

    Args:
        n_fft: FFT window size shared by spectrum / harmony / timbre.
        hop_length: Hop between frames, shared across most extractors.
        n_mfcc: Number of MFCC coefficients.
        prefer_essentia: Prefer essentia's RhythmExtractor2013 for tempo
                         when installed; falls back to librosa otherwise.
    """

    def __init__(
        self,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mfcc: int = 13,
        prefer_essentia: bool = True,
    ) -> None:
        self._n_fft = n_fft
        self._hop_length = hop_length

        # Sub-extractors: instantiated once, reused per call.
        self._tempo = TempoExtractor(prefer_essentia=prefer_essentia, hop_length=hop_length)
        self._pitch = PitchExtractor(hop_length=hop_length)
        self._spectrum = SpectrumExtractor(n_fft=n_fft, hop_length=hop_length)
        self._timbre = TimbreExtractor(n_mfcc=n_mfcc, n_fft=n_fft, hop_length=hop_length)
        self._loudness = LoudnessExtractor(frame_length=n_fft, hop_length=hop_length)
        self._harmony = HarmonyExtractor(n_fft=n_fft, hop_length=hop_length)
        self._stereo = StereoExtractor()
        self._rhythm = RhythmExtractor(hop_length=hop_length)
        self._hpss = HPSSExtractor()

    # ------------------------------------------------------------------ public
    def extract(self, audio: AudioData) -> FeatureVector:
        """Extract every feature domain from a pre-loaded :class:`AudioData`.

        Args:
            audio: Pre-loaded audio buffer.  Use :class:`audio.AudioLoader`
                   to produce one from a file, or construct directly from a
                   numpy array for tests.

        Raises:
            ValueError: if ``audio.samplerate`` is not positive.
        """
        if audio.samplerate <= 0:
            raise ValueError(f"invalid samplerate: {audio.samplerate}")

        y_multi = audio.samples
        sr = audio.samplerate
        channels = audio.channels

        # Mono view for extractors that don't need stereo channels.
        if y_multi.ndim == 1:
            y_mono = y_multi.astype(np.float32, copy=False)
        else:
            y_mono = librosa.to_mono(y_multi).astype(np.float32, copy=False)

        source_str = str(audio.source_path) if audio.source_path is not None else ""
        logger.info(
            "Extracting features (sr=%d, ch=%d, dur=%.2fs)",
            sr, channels, audio.duration,
        )

        return FeatureVector(
            source_path=source_str,
            samplerate=int(sr),
            duration_sec=float(audio.duration),
            channels=channels,
            tempo=self._tempo.extract(y_mono, sr),
            pitch=self._pitch.extract(y_mono, sr),
            spectrum=self._spectrum.extract(y_mono, sr),
            loudness=self._loudness.extract(y_multi, sr),
            harmony=self._harmony.extract(y_mono, sr),
            timbre=self._timbre.extract(y_mono, sr),
            stereo=self._stereo.extract(y_multi, sr),
            rhythm=self._rhythm.extract(y_mono, sr),
            hpss=self._hpss.extract(y_mono, sr),
        )


# =============================================================================
#  Serialization helpers
# =============================================================================

def _dataclass_to_dict(
    obj: Any, *, skip_fields: tuple[str, ...] = ()
) -> dict[str, Any]:
    """Convert a dataclass instance to a JSON-safe dict.

    Recursively walks ``obj``: nested dataclasses become dicts, ndarrays become
    Python lists, numpy scalars become native ints/floats.  Fields whose name
    is in ``skip_fields`` are dropped entirely (used for ``include_arrays=False``).
    """
    out: dict[str, Any] = {}
    for f in fields(obj):
        if f.name in skip_fields:
            continue
        out[f.name] = _to_json_safe(getattr(obj, f.name))
    return out


def _to_json_safe(value: Any) -> Any:
    """Recursively convert numpy / dataclass values to plain JSON types."""
    if value is None:
        return None
    if isinstance(value, np.ndarray):
        # Preserve NaN as null; JSON can't encode NaN by default in strict mode
        # but json.dump lets it through as `NaN` — we normalize to None instead
        # so consumers on other stacks (JS, Rust) don't choke.
        if value.dtype.kind == "f":
            return [None if not np.isfinite(v) else float(v) for v in value.tolist()] \
                if value.ndim == 1 else value.tolist()
        return value.tolist()
    if isinstance(value, (np.floating, np.integer, np.bool_)):
        return value.item()
    if isinstance(value, float) and not np.isfinite(value):
        return None
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        return _dataclass_to_dict(value)
    if isinstance(value, (list, tuple)):
        return [_to_json_safe(x) for x in value]
    if isinstance(value, dict):
        return {k: _to_json_safe(v) for k, v in value.items()}
    if isinstance(value, Path):
        return str(value)
    return value


def _f32(seq: Any) -> NDArray[np.float32]:
    """Cast a JSON list back to an ndarray of float32 (NaN-aware)."""
    if seq is None:
        return np.zeros(0, dtype=np.float32)
    # None values (from _to_json_safe's NaN normalization) → NaN
    arr = np.asarray(
        [np.nan if x is None else x for x in seq] if isinstance(seq, list) and
        seq and not isinstance(seq[0], list) else seq,
        dtype=np.float32,
    )
    return arr


# ---- Per-group rebuilders for FeatureVector.from_dict -----------------------
# Each ``_build_*`` restores one sub-dataclass, handling missing (dropped)
# array fields by substituting empty arrays of the correct dtype.

def _build_tempo(d: dict[str, Any]) -> TempoFeatures:
    return TempoFeatures(
        bpm=float(d["bpm"]),
        beat_times_sec=_f32(d.get("beat_times_sec", [])),
        beat_confidence=float(d["beat_confidence"]),
        backend=str(d["backend"]),
    )


def _build_pitch(d: dict[str, Any]) -> PitchFeatures:
    return PitchFeatures(
        f0_hz=_f32(d.get("f0_hz", [])),
        f0_median_hz=float(d["f0_median_hz"]),
        voiced_ratio=float(d["voiced_ratio"]),
        pitch_histogram=_f32(d["pitch_histogram"]),
    )


def _build_spectrum(d: dict[str, Any]) -> SpectrumFeatures:
    return SpectrumFeatures(
        fft_magnitude=_f32(d.get("fft_magnitude", [])),
        stft_magnitude=np.asarray(d.get("stft_magnitude", [[]]), dtype=np.float32),
        spectral_centroid=_f32(d.get("spectral_centroid", [])),
        spectral_bandwidth=_f32(d.get("spectral_bandwidth", [])),
        spectral_contrast=np.asarray(d.get("spectral_contrast", [[]]), dtype=np.float32),
        spectral_flatness=_f32(d.get("spectral_flatness", [])),
        spectral_rolloff=_f32(d.get("spectral_rolloff", [])),
        spectral_centroid_mean=float(d["spectral_centroid_mean"]),
        spectral_bandwidth_mean=float(d["spectral_bandwidth_mean"]),
        spectral_contrast_mean=_f32(d["spectral_contrast_mean"]),
        spectral_flatness_mean=float(d["spectral_flatness_mean"]),
        spectral_rolloff_mean=float(d["spectral_rolloff_mean"]),
        n_fft=int(d["n_fft"]),
        hop_length=int(d["hop_length"]),
    )


def _build_loudness(d: dict[str, Any]) -> LoudnessFeatures:
    return LoudnessFeatures(
        rms_db=float(d["rms_db"]),
        peak_dbfs=float(d["peak_dbfs"]),
        dynamic_range_db=float(d["dynamic_range_db"]),
        lufs_integrated=float("-inf") if d["lufs_integrated"] is None else float(d["lufs_integrated"]),
        rms_envelope=_f32(d.get("rms_envelope", [])),
    )


def _build_harmony(d: dict[str, Any]) -> HarmonyFeatures:
    return HarmonyFeatures(
        chroma=np.asarray(d.get("chroma", [[]]), dtype=np.float32),
        chroma_mean=_f32(d["chroma_mean"]),
        tonnetz=np.asarray(d.get("tonnetz", [[]]), dtype=np.float32),
        tonnetz_mean=_f32(d["tonnetz_mean"]),
    )


def _build_timbre(d: dict[str, Any]) -> TimbreFeatures:
    return TimbreFeatures(
        mfcc=np.asarray(d.get("mfcc", [[]]), dtype=np.float32),
        mfcc_mean=_f32(d["mfcc_mean"]),
        mfcc_std=_f32(d["mfcc_std"]),
        mfcc_delta=np.asarray(d.get("mfcc_delta", [[]]), dtype=np.float32),
        mfcc_delta_mean=_f32(d["mfcc_delta_mean"]),
        n_mfcc=int(d["n_mfcc"]),
    )


def _build_stereo(d: dict[str, Any]) -> StereoFeatures:
    return StereoFeatures(
        stereo_width=float(d["stereo_width"]),
        lr_balance=float(d["lr_balance"]),
        lr_correlation=float(d["lr_correlation"]),
        is_stereo=bool(d["is_stereo"]),
    )


def _build_rhythm(d: dict[str, Any]) -> RhythmFeatures:
    return RhythmFeatures(
        onset_strength=_f32(d.get("onset_strength", [])),
        onset_times_sec=_f32(d["onset_times_sec"]),
        onset_rate_per_sec=float(d["onset_rate_per_sec"]),
        hop_length=int(d["hop_length"]),
    )


def _build_hpss(d: dict[str, Any]) -> HPSSFeatures:
    return HPSSFeatures(
        harmonic_energy=float(d["harmonic_energy"]),
        percussive_energy=float(d["percussive_energy"]),
        harmonic_ratio=float(d["harmonic_ratio"]),
        percussive_ratio=float(d["percussive_ratio"]),
        harmonic_energy_db=float(d["harmonic_energy_db"]),
        percussive_energy_db=float(d["percussive_energy_db"]),
    )
