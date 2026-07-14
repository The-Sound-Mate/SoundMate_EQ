"""Tempo features — BPM (beats per minute) and beat positions.

Uses essentia's ``RhythmExtractor2013`` when available (typically the most
accurate for music), and falls back to ``librosa.beat.beat_track`` otherwise.
essentia has no official Windows wheels, so the fallback is what most Windows
users will actually hit.
"""
from __future__ import annotations

from dataclasses import dataclass

import librosa
import numpy as np
from numpy.typing import NDArray

# ---- Optional essentia import ------------------------------------------------
# essentia is included only in the ``essentia`` optional-dependency group.
# If it is not installed we silently fall back to librosa.
try:  # pragma: no cover - environment dependent
    import essentia.standard as es_std  # type: ignore[import-not-found]

    _HAS_ESSENTIA: bool = True
except ImportError:  # pragma: no cover
    es_std = None  # type: ignore[assignment]
    _HAS_ESSENTIA = False


@dataclass(frozen=True, slots=True)
class TempoFeatures:
    """Tempo descriptors extracted from a mono audio signal.

    Attributes:
        bpm: Estimated global tempo in beats-per-minute.
        beat_times_sec: Detected beat positions in seconds, shape ``(num_beats,)``.
        beat_confidence: Estimator confidence in ``[0, 1]``.  librosa does not
            expose a direct confidence, so the fallback returns ``0.0``.
        backend: Which backend produced the result — ``"essentia"`` or ``"librosa"``.
    """

    bpm: float
    beat_times_sec: NDArray[np.float32]
    beat_confidence: float
    backend: str


class TempoExtractor:
    """Compute BPM and beat positions from mono audio.

    Args:
        prefer_essentia: If ``True`` (default) and essentia is importable, use
            ``RhythmExtractor2013(method="multifeature")``.  Otherwise use
            ``librosa.beat.beat_track``.
        hop_length: Hop size used by the librosa backend.
    """

    def __init__(self, prefer_essentia: bool = True, hop_length: int = 512) -> None:
        self._use_essentia: bool = prefer_essentia and _HAS_ESSENTIA
        self._hop_length: int = hop_length

    # ------------------------------------------------------------------ public
    def extract(self, y: NDArray[np.floating], sr: int) -> TempoFeatures:
        """Extract tempo features from a 1D mono signal ``y`` at rate ``sr``."""
        if self._use_essentia:
            return self._extract_essentia(y, sr)
        return self._extract_librosa(y, sr)

    # -------------------------------------------------------------- essentia
    def _extract_essentia(self, y: NDArray[np.floating], sr: int) -> TempoFeatures:
        """essentia's ``RhythmExtractor2013`` — multifeature = most accurate mode."""
        assert es_std is not None  # narrowed by _use_essentia

        # essentia expects mono float32.  It is trained on 44.1 kHz, so resample
        # if the caller loaded at a different rate.
        if sr != 44100:
            y_res = librosa.resample(y.astype(np.float32), orig_sr=sr, target_sr=44100)
        else:
            y_res = y.astype(np.float32)

        rhythm = es_std.RhythmExtractor2013(method="multifeature")
        # Returns: (bpm, beat_times, confidence, _estimates, beat_intervals)
        bpm, beats, confidence, _, _ = rhythm(y_res)

        # essentia confidence is on a 0..~5.4 scale; clamp to [0, 1] for consistency
        conf_norm = float(min(max(confidence / 5.4, 0.0), 1.0))

        return TempoFeatures(
            bpm=float(bpm),
            beat_times_sec=np.asarray(beats, dtype=np.float32),
            beat_confidence=conf_norm,
            backend="essentia",
        )

    # -------------------------------------------------------------- librosa
    def _extract_librosa(self, y: NDArray[np.floating], sr: int) -> TempoFeatures:
        """librosa fallback — dynamic-programming beat tracker."""
        tempo, beat_frames = librosa.beat.beat_track(
            y=y, sr=sr, hop_length=self._hop_length, units="frames"
        )
        beat_times = librosa.frames_to_time(
            beat_frames, sr=sr, hop_length=self._hop_length
        )

        # librosa returns tempo as a 0-D or 1-D ndarray depending on version;
        # coerce to a plain float.
        bpm = float(np.atleast_1d(tempo)[0])

        return TempoFeatures(
            bpm=bpm,
            beat_times_sec=np.asarray(beat_times, dtype=np.float32),
            beat_confidence=0.0,  # librosa does not expose confidence
            backend="librosa",
        )
