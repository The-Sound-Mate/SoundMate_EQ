"""Tag types — the intensity-weighted equivalent of "genre labels".

Each :class:`Tag` carries a name, a :class:`TagCategory`, and a continuous
intensity in ``[0.0, 1.0]``.  A :class:`TagSet` groups tags produced for one
input file and provides ergonomic accessors (flat dict, category grouping,
JSON dump matching the user's example format).

Example output shape ::

    {
        "bass_heavy": 0.82,
        "warm":       0.64,
        "bright":     0.31,
        "wide":       0.91
    }
"""
from __future__ import annotations

import json
from collections.abc import Iterator
from dataclasses import dataclass
from enum import Enum
from pathlib import Path


class TagCategory(str, Enum):
    """High-level tag namespaces.

    Values are lowercase strings so YAML configs and JSON dumps stay readable.
    """

    BASS = "bass"
    MID = "mid"
    TREBLE = "treble"
    VOCAL = "vocal"
    DRUM = "drum"
    SPACE = "space"
    DYNAMICS = "dynamics"
    TONE = "tone"
    MIX = "mix"
    INSTRUMENT_FOCUS = "instrument_focus"


@dataclass(frozen=True, slots=True)
class Tag:
    """A single tag with a confidence in ``[0.0, 1.0]``.

    Attributes:
        name: Unique identifier such as ``"bass_heavy"`` or ``"warm"``.
        category: Which namespace the tag belongs to.
        intensity: Confidence in ``[0.0, 1.0]``.  0 = definitely absent,
            1 = fully present.  Values are always clipped by the generator.
    """

    name: str
    category: TagCategory
    intensity: float


@dataclass(frozen=True, slots=True)
class TagSet:
    """Immutable collection of tags produced for one input.

    Tags are stored in insertion order (i.e., registry order).  For a
    sorted-by-intensity view use :meth:`as_dict` (default) or :meth:`top`.
    """

    tags: tuple[Tag, ...]

    # ---------------------------------------------------------------- accessors
    def as_dict(self, *, sort_by_intensity: bool = True) -> dict[str, float]:
        """Return ``{name: intensity}`` — matches the user-facing example format.

        By default the dict is ordered by descending intensity so the most
        confident tags come first when iterating.
        """
        items = [(t.name, float(t.intensity)) for t in self.tags]
        if sort_by_intensity:
            items.sort(key=lambda kv: -kv[1])
        return dict(items)

    def grouped(self) -> dict[str, dict[str, float]]:
        """Return ``{category: {name: intensity}}`` — a hierarchical view."""
        out: dict[str, dict[str, float]] = {}
        for t in self.tags:
            out.setdefault(t.category.value, {})[t.name] = float(t.intensity)
        return out

    def by_category(self, category: TagCategory) -> tuple[Tag, ...]:
        """All tags in ``category``, in registration order."""
        return tuple(t for t in self.tags if t.category is category)

    def top(self, n: int = 5) -> tuple[Tag, ...]:
        """The ``n`` highest-intensity tags across all categories."""
        return tuple(sorted(self.tags, key=lambda t: -t.intensity)[:n])

    def get(self, name: str) -> Tag | None:
        """Return the tag by name, or ``None`` if absent."""
        for t in self.tags:
            if t.name == name:
                return t
        return None

    def __getitem__(self, name: str) -> float:
        """``tags["bass_heavy"]`` → intensity, or 0.0 if the tag is missing."""
        t = self.get(name)
        return float(t.intensity) if t is not None else 0.0

    def __iter__(self) -> Iterator[Tag]:
        return iter(self.tags)

    def __len__(self) -> int:
        return len(self.tags)

    # ---------------------------------------------------------------- export
    def to_json(
        self,
        path: str | Path,
        *,
        indent: int | None = 2,
        grouped: bool = False,
    ) -> Path:
        """Write tags to a JSON file.

        Args:
            path: Output file path.
            indent: JSON indentation (``None`` = compact single-line).
            grouped: If True, use the ``{category: {name: intensity}}`` shape.
                     If False (default), use the flat ``{name: intensity}``
                     shape matching the user example.
        """
        out = Path(path)
        out.parent.mkdir(parents=True, exist_ok=True)
        payload = self.grouped() if grouped else self.as_dict()
        with out.open("w", encoding="utf-8") as f:
            json.dump(payload, f, indent=indent, ensure_ascii=False)
        return out
