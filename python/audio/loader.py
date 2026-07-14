"""Audio file loading — MP3 / WAV / FLAC / etc → :class:`AudioData`.

This is **step 2** of the pipeline: given a path, decode the file and return
an in-memory :class:`AudioData` object.  The rest of the pipeline
(feature extraction, tagging, EQ) takes :class:`AudioData` as input, never
paths — this makes the boundary explicit and each stage independently testable.

The default :class:`AudioLoader` delegates decoding to librosa (which itself
uses ``soundfile`` for WAV/FLAC/OGG and ``audioread`` for MP3/M4A/etc.),
so any format either library supports works.

Design notes
------------
- ``AudioData`` is a frozen dataclass so downstream code cannot mutate the
  buffer accidentally.
- Fatal I/O and decode errors surface as :class:`FileNotFoundError` or
  :class:`AudioLoadError` — never generic ``Exception``.
- Loader logs a single ``INFO`` line per successful load and a ``DEBUG``
  line with buffer details, so ``main.py``'s logging level controls verbosity.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol, runtime_checkable

import librosa
import numpy as np
from numpy.typing import NDArray

logger = logging.getLogger(__name__)


# =============================================================================
#  Errors
# =============================================================================

class AudioLoadError(Exception):
    """Raised when an audio file exists but cannot be decoded.

    File-not-found is signalled with the stdlib :class:`FileNotFoundError`
    instead so callers can distinguish "wrong path" from "corrupt file".
    """


# =============================================================================
#  AudioData — the immutable buffer the rest of the pipeline consumes
# =============================================================================

@dataclass(frozen=True, slots=True)
class AudioData:
    """Immutable audio buffer + metadata.

    Attributes:
        samples: Float32 samples in ``[-1.0, 1.0]``.
            Mono → shape ``(num_frames,)``.
            Multi-channel → shape ``(channels, num_frames)``.
        samplerate: Sample rate in Hz.
        channels: Channel count (1 = mono, 2 = stereo, ...).
        source_path: Origin file path.  ``None`` for synthesized / streamed
            buffers (useful in tests where you build ``AudioData`` from a
            numpy array without touching the filesystem).
    """

    samples: NDArray[np.float32]
    samplerate: int
    channels: int
    source_path: Path | None = None

    # ------------------------------------------------------------------ shape
    @property
    def num_frames(self) -> int:
        """Total frames per channel."""
        if self.samples.ndim == 1:
            return int(self.samples.shape[0])
        return int(self.samples.shape[-1])

    @property
    def duration(self) -> float:
        """Duration in seconds.  Returns 0.0 for zero-rate / empty buffers."""
        if self.samplerate <= 0:
            return 0.0
        return self.num_frames / float(self.samplerate)

    @property
    def is_mono(self) -> bool:
        """True if ``channels == 1``."""
        return self.channels == 1

    # ------------------------------------------------------------ operations
    def to_mono(self) -> "AudioData":
        """Return a mono copy (or ``self`` if already mono)."""
        if self.is_mono:
            return self
        mono = librosa.to_mono(self.samples).astype(np.float32, copy=False)
        return AudioData(
            samples=mono,
            samplerate=self.samplerate,
            channels=1,
            source_path=self.source_path,
        )


# =============================================================================
#  AudioLoader — the concrete loader
# =============================================================================

@runtime_checkable
class AudioLoaderProtocol(Protocol):
    """Structural type for anything that maps a path to :class:`AudioData`.

    Useful when injecting a mock loader into tests or providing an alternative
    backend (e.g., an in-process streaming reader for large files).
    """

    def load(self, path: str | Path) -> AudioData: ...


class AudioLoader:
    """librosa-backed loader — decodes MP3, WAV, FLAC, OGG, M4A, AIFF, etc.

    Args:
        target_sr: Resample everything to this rate at load time.
                   Pass ``None`` to keep the file's native rate.  Default
                   22050 matches librosa's default and is fine for MIR.
        mono: If ``True``, mix down to mono at load time.  If ``False``
              (default), preserve channels so stereo features stay meaningful.

    Example ::

        loader = AudioLoader(target_sr=44100)
        audio = loader.load("track.mp3")
        audio.duration          # → 213.47
        audio.samples.shape     # → (2, 9413728)   ← stereo preserved
    """

    def __init__(
        self,
        target_sr: int | None = 22050,
        mono: bool = False,
    ) -> None:
        self._target_sr = target_sr
        self._mono = mono

    # ---------------------------------------------------------------- public
    def load(self, path: str | Path) -> AudioData:
        """Load an audio file into :class:`AudioData`.

        Raises:
            FileNotFoundError: ``path`` does not exist or is not a file.
            AudioLoadError: file exists but the decoder rejected it.
        """
        path = Path(path)

        # ---- Explicit path validation before touching a decoder ------------
        # A clear FileNotFoundError is more useful than the "audioread failed
        # to find a backend" trace librosa would otherwise produce.
        if not path.exists():
            raise FileNotFoundError(f"Audio file not found: {path}")
        if not path.is_file():
            raise FileNotFoundError(f"Not a regular file: {path}")

        logger.info("Loading audio: %s", path)

        # ---- Decode ---------------------------------------------------------
        try:
            samples, sr = librosa.load(
                str(path),
                sr=self._target_sr,
                mono=self._mono,
            )
        except Exception as exc:  # librosa/audioread raise many different types
            raise AudioLoadError(
                f"Failed to decode {path}: {type(exc).__name__}: {exc}"
            ) from exc

        # librosa returns float32 already, but be explicit.
        samples = np.asarray(samples, dtype=np.float32)

        # Channel count is derived from shape — librosa returns (N,) for mono
        # and (C, N) for multi-channel.
        channels = 1 if samples.ndim == 1 else int(samples.shape[0])
        frames = samples.shape[-1] if samples.ndim > 1 else samples.shape[0]

        logger.debug(
            "Decoded %s: %d frames @ %d Hz, %d ch, %.2f s",
            path.name, frames, sr, channels, frames / sr if sr else 0.0,
        )

        return AudioData(
            samples=samples,
            samplerate=int(sr),
            channels=channels,
            source_path=path,
        )


# =============================================================================
#  Backward-compatible aliases
# =============================================================================
# Older parts of the codebase referred to the Protocol as ``AudioLoader``.
# The concrete class now owns that name; the Protocol lives on as
# ``AudioLoaderProtocol`` for type-hint use.
