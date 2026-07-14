"""EQ preset data types + YAML loader with optional auto-reload.

The system uses a **fixed 10-band graphic EQ** at ISO octave centers:

    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000  (Hz)

Each preset is keyed by a tag name and maps a **subset** of those frequencies
to gains in dB.  Missing bands are implicitly 0 dB.  This matches the compact
YAML format the user asked for::

    bass_heavy:
        31: -3
        62: -2
        125: -1

    bright:
        4000: -2
        8000: -2

Presets can be reloaded from disk without restarting: pass
``auto_reload=True`` to :meth:`PresetStore.load` and every subsequent
:meth:`get` call checks the file's mtime and re-parses when it changes.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml


# =============================================================================
#  Fixed 10-band frequency layout — the whole EQ system revolves around this
# =============================================================================

BAND_FREQUENCIES_HZ: tuple[int, ...] = (
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000,
)


def is_valid_band(frequency_hz: int) -> bool:
    """True if ``frequency_hz`` is one of the 10 canonical band centers."""
    return frequency_hz in BAND_FREQUENCIES_HZ


# =============================================================================
#  Dataclasses
# =============================================================================

@dataclass(frozen=True, slots=True)
class EQBand:
    """One band of the fixed 10-band graphic EQ.

    Attributes:
        frequency_hz: Must be one of :data:`BAND_FREQUENCIES_HZ`.
        gain_db: Boost (+) or cut (−) in dB.
    """

    frequency_hz: int
    gain_db: float


@dataclass(frozen=True, slots=True)
class EQPreset:
    """Correction curve associated with a single tag.

    Always exposes all 10 bands in :data:`BAND_FREQUENCIES_HZ` order — bands
    the YAML author left out are stored as 0 dB so downstream code never has
    to special-case "missing" bands.

    Attributes:
        tag_name: Which tag this preset corrects (matches ``Tag.name``).
        bands: Exactly 10 bands, in canonical frequency order.
    """

    tag_name: str
    bands: tuple[EQBand, ...]

    # ------------------------------------------------------------------ query
    def gain_at(self, frequency_hz: int) -> float:
        """Look up gain for a given band frequency.

        Raises:
            ValueError: if ``frequency_hz`` is not one of the 10 canonical bands.
        """
        for b in self.bands:
            if b.frequency_hz == frequency_hz:
                return b.gain_db
        raise ValueError(
            f"{frequency_hz} Hz is not one of the standard bands "
            f"{list(BAND_FREQUENCIES_HZ)}"
        )

    def gains(self) -> tuple[float, ...]:
        """Ten gains in canonical frequency order — handy for vectorized math."""
        return tuple(b.gain_db for b in self.bands)

    def as_dict(self) -> dict[int, float]:
        """``{frequency_hz: gain_db}`` — human-readable dump."""
        return {b.frequency_hz: b.gain_db for b in self.bands}

    def is_flat(self) -> bool:
        """True if every band is 0 dB — i.e. the preset applies no correction."""
        return all(b.gain_db == 0.0 for b in self.bands)

    # ------------------------------------------------------------ construction
    @classmethod
    def from_gains(
        cls, tag_name: str, gains_by_freq: dict[int, float]
    ) -> "EQPreset":
        """Build a preset from a partial ``{freq_hz: gain_db}`` map.

        Frequencies not in :data:`BAND_FREQUENCIES_HZ` raise ``ValueError`` so
        typos in YAML fail loudly (e.g. writing ``60`` instead of ``62``).
        Bands not specified in the input default to 0.0 dB.
        """
        for freq in gains_by_freq:
            if not is_valid_band(freq):
                raise ValueError(
                    f"tag '{tag_name}': frequency {freq} Hz is not part of the "
                    f"10-band layout {list(BAND_FREQUENCIES_HZ)}"
                )
        bands = tuple(
            EQBand(frequency_hz=f, gain_db=float(gains_by_freq.get(f, 0.0)))
            for f in BAND_FREQUENCIES_HZ
        )
        return cls(tag_name=tag_name, bands=bands)

    @classmethod
    def flat(cls, tag_name: str) -> "EQPreset":
        """A no-op preset with all bands at 0 dB."""
        return cls.from_gains(tag_name, {})


# =============================================================================
#  Store — the interface the mixer / engine interact with
# =============================================================================

class PresetStore:
    """In-memory store of :class:`EQPreset` keyed by tag name.

    Two independent responsibilities live on this class:

    1. **Fast lookup** via :meth:`get` / :meth:`has` / :meth:`all`.
    2. **Live-reload** when loaded from a file with ``auto_reload=True`` —
       ``get()`` checks the file's mtime and re-parses on change, so editing
       ``config/eq_presets.yaml`` takes effect without restarting the process.

    The class is intentionally *not* a frozen dataclass because the auto-reload
    path needs to mutate ``_presets`` in place.
    """

    def __init__(
        self,
        presets: dict[str, EQPreset],
        *,
        path: Path | None = None,
        auto_reload: bool = False,
    ) -> None:
        self._presets: dict[str, EQPreset] = dict(presets)
        self._path: Path | None = Path(path) if path else None
        self._auto_reload: bool = auto_reload
        # Snapshot the initial mtime so the first reload check has a baseline.
        self._mtime: float = self._current_mtime()

    # -------------------------------------------------------------- accessors
    def get(self, tag_name: str) -> EQPreset | None:
        """Return the preset for ``tag_name`` or ``None`` if none is defined."""
        if self._auto_reload:
            self._maybe_reload()
        return self._presets.get(tag_name)

    def has(self, tag_name: str) -> bool:
        if self._auto_reload:
            self._maybe_reload()
        return tag_name in self._presets

    def all(self) -> tuple[EQPreset, ...]:
        """Snapshot of every preset currently in the store."""
        if self._auto_reload:
            self._maybe_reload()
        return tuple(self._presets.values())

    def names(self) -> tuple[str, ...]:
        if self._auto_reload:
            self._maybe_reload()
        return tuple(self._presets.keys())

    def __len__(self) -> int:
        return len(self._presets)

    def __contains__(self, tag_name: str) -> bool:
        return self.has(tag_name)

    # ---------------------------------------------------------------- reload
    def reload(self) -> None:
        """Force a re-parse of the source file, whether stale or not.

        Silently no-ops if this store wasn't constructed from a file
        (e.g. an in-memory ``PresetStore({...})``).
        """
        if self._path is None:
            return
        self._presets = _parse_yaml_to_presets(_read_yaml(self._path))
        self._mtime = self._current_mtime()

    def _maybe_reload(self) -> None:
        """Reload if the file has been modified since our last read."""
        if self._path is None:
            return
        try:
            mtime = self._current_mtime()
        except OSError:
            # File was moved or deleted mid-run — keep our in-memory copy.
            return
        if mtime > self._mtime:
            try:
                self.reload()
            except (OSError, yaml.YAMLError):
                # Bad edit mid-save; keep the last-known-good presets.
                # Reset mtime so we retry on the next tick.
                self._mtime = mtime

    def _current_mtime(self) -> float:
        if self._path is None or not self._path.exists():
            return 0.0
        return self._path.stat().st_mtime

    # -------------------------------------------------------------- factories
    @classmethod
    def load(
        cls,
        path: str | Path,
        *,
        auto_reload: bool = False,
    ) -> "PresetStore":
        """Load presets from a YAML file.

        Args:
            path: Path to a YAML file in the format documented in the module
                  docstring.
            auto_reload: If True, subsequent :meth:`get` calls will re-parse
                         the file whenever its mtime changes on disk.
        """
        path = Path(path)
        presets = _parse_yaml_to_presets(_read_yaml(path))
        return cls(presets, path=path, auto_reload=auto_reload)

    @classmethod
    def default_path(cls) -> Path:
        """Path to the shipped ``config/eq_presets.yaml``."""
        # <project>/eq/presets.py → <project>/config/eq_presets.yaml
        return Path(__file__).resolve().parent.parent / "config" / "eq_presets.yaml"

    @classmethod
    def load_default(cls, *, auto_reload: bool = False) -> "PresetStore":
        """Convenience: load the shipped default preset file."""
        return cls.load(cls.default_path(), auto_reload=auto_reload)


# =============================================================================
#  YAML parsing helpers
# =============================================================================

def _read_yaml(path: Path) -> dict[str, Any]:
    """Read a YAML file into a dict, returning ``{}`` for empty files."""
    with path.open("r", encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    if not isinstance(data, dict):
        raise ValueError(f"{path}: top level must be a mapping (tag → gains)")
    return data


def _parse_yaml_to_presets(raw: dict[str, Any]) -> dict[str, EQPreset]:
    """Convert a parsed YAML dict → ``{tag_name: EQPreset}``.

    Supports the compact user format (``tag: {31: -3, 62: -2}``) and, for
    forward compatibility, a wrapped form (``tag: {bands: {31: -3, ...}}``).
    Non-numeric keys inside a preset block (e.g. ``description``) are ignored
    so YAML authors can add comments-as-values without breaking parsing.
    """
    presets: dict[str, EQPreset] = {}

    for tag_name, spec in raw.items():
        # Top-level keys like `version: 1` have non-dict values — skip.
        if not isinstance(spec, dict):
            continue

        # Extract the band map: either `spec["bands"]` or `spec` itself.
        if "bands" in spec and isinstance(spec["bands"], dict):
            band_source: dict[Any, Any] = spec["bands"]
        else:
            band_source = spec

        gains_by_freq: dict[int, float] = {}
        for key, value in band_source.items():
            try:
                freq_int = int(key)
            except (TypeError, ValueError):
                # Non-numeric key (metadata like "description") — ignore.
                continue
            try:
                gain_float = float(value)
            except (TypeError, ValueError):
                raise ValueError(
                    f"tag '{tag_name}': band {freq_int} Hz has non-numeric "
                    f"gain {value!r}"
                ) from None
            gains_by_freq[freq_int] = gain_float

        presets[str(tag_name)] = EQPreset.from_gains(str(tag_name), gains_by_freq)

    return presets
