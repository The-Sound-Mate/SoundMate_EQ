"""Audio I/O layer — file → :class:`AudioData`.

Public entry points::

    from audio import AudioLoader, AudioData
    audio = AudioLoader().load("track.mp3")
    audio.duration   # seconds
    audio.samples    # numpy float32, shape (N,) mono or (C, N) multi-channel

:class:`AudioData` is a frozen dataclass and safe to import without the audio
stack; :class:`AudioLoader` needs librosa at import time so importing this
package requires it.
"""
from __future__ import annotations

from audio.loader import (
    AudioData,
    AudioLoadError,
    AudioLoader,
    AudioLoaderProtocol,
)

__all__ = [
    "AudioData",
    "AudioLoadError",
    "AudioLoader",
    "AudioLoaderProtocol",
]
