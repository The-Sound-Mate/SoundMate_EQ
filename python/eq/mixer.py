"""Mix per-tag EQ presets into the Final EQ.

Formula
-------
    Final EQ[band]  =  Σ over tags  ( tag.intensity  ×  preset[tag].gain[band] )

    then     clip to [min_gain_db, max_gain_db] per band
    then     preamp_db = -( max(0, max_boost) + preamp_headroom_db )

Same bands from different tag presets are **summed** — three tags each cutting
62 Hz by −2 dB × their intensity produce a stronger cut than any one alone.
The per-band clip prevents any single band from running away when many tags
pile up on top of each other.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field, fields
from pathlib import Path

from eq.presets import BAND_FREQUENCIES_HZ, EQPreset, PresetStore
from tagging.tags import TagSet


# =============================================================================
#  Per-tag traceability record
# =============================================================================

@dataclass(frozen=True, slots=True)
class EQContribution:
    """One tag's contribution to the final mix — kept for UI / debugging.

    Attributes:
        tag_name: The tag whose preset contributed.
        tag_intensity: The tag's ``[0, 1]`` score used as the weight.
        preset: The preset that was applied (kept by reference).
        scaled_gains: Per-band gains after multiplying by ``tag_intensity``.
                      Ten floats in :data:`BAND_FREQUENCIES_HZ` order.
    """

    tag_name: str
    tag_intensity: float
    preset: EQPreset
    scaled_gains: tuple[float, ...]


# =============================================================================
#  FinalEQ — the requested dataclass with one explicit field per frequency
# =============================================================================

# Field-name ↔ Hz map. Derived from BAND_FREQUENCIES_HZ so the two can't drift.
_HZ_FIELDS: tuple[tuple[str, int], ...] = tuple(
    (f"hz_{hz}", hz) for hz in BAND_FREQUENCIES_HZ
)


@dataclass(frozen=True, slots=True)
class FinalEQ:
    """Final 10-band EQ — one explicit float field per ISO octave center.

    Field naming: ``hz_<frequency>`` — reads as "at <frequency> Hz".
    All band values are gains in dB (positive = boost, negative = cut).

    ``preamp_db`` is a negative offset that should be applied *before* the
    EQ chain to prevent clipping when any band boosts.  It is ``0.0`` when
    the resulting curve only cuts.

    ``contributions`` is a per-tag audit trail — which tags fed how much into
    which bands — useful for UI ("why did we cut 62 Hz?").
    """

    # -- The 10 band gains (dB) — this is the user-facing shape --------------
    hz_31: float = 0.0
    hz_62: float = 0.0
    hz_125: float = 0.0
    hz_250: float = 0.0
    hz_500: float = 0.0
    hz_1000: float = 0.0
    hz_2000: float = 0.0
    hz_4000: float = 0.0
    hz_8000: float = 0.0
    hz_16000: float = 0.0

    # -- Metadata ------------------------------------------------------------
    preamp_db: float = 0.0
    contributions: tuple[EQContribution, ...] = field(default_factory=tuple)

    # ---------------------------------------------------------------- queries
    def gains(self) -> tuple[float, ...]:
        """The 10 band gains in canonical frequency order.

        Convenient for feeding a numeric audio engine that expects a
        fixed-length gain vector.
        """
        return tuple(getattr(self, name) for name, _ in _HZ_FIELDS)

    def as_dict(self) -> dict[int, float]:
        """``{frequency_hz: gain_db}`` view of the 10 bands (excludes preamp)."""
        return {hz: float(getattr(self, name)) for name, hz in _HZ_FIELDS}

    def gain_at(self, frequency_hz: int) -> float:
        """Look up gain at a canonical band frequency.

        Raises:
            ValueError: if ``frequency_hz`` is not one of the 10 bands.
        """
        for name, hz in _HZ_FIELDS:
            if hz == frequency_hz:
                return float(getattr(self, name))
        raise ValueError(
            f"{frequency_hz} Hz is not one of "
            f"{[hz for _, hz in _HZ_FIELDS]}"
        )

    def is_flat(self) -> bool:
        """True if every band is exactly 0 dB — no correction to apply."""
        return all(g == 0.0 for g in self.gains())

    # ----------------------------------------------------------- construction
    @classmethod
    def from_gains(
        cls,
        gains: dict[int, float] | tuple[float, ...] | list[float],
        preamp_db: float = 0.0,
        contributions: tuple[EQContribution, ...] = (),
    ) -> "FinalEQ":
        """Build from either a ``{freq: gain}`` dict or a 10-tuple.

        Dict form fills missing bands with 0 dB.  Tuple/list form must have
        exactly 10 elements in :data:`BAND_FREQUENCIES_HZ` order.
        """
        if isinstance(gains, dict):
            kwargs = {name: float(gains.get(hz, 0.0)) for name, hz in _HZ_FIELDS}
        else:
            gains_seq = tuple(gains)
            if len(gains_seq) != len(_HZ_FIELDS):
                raise ValueError(
                    f"expected {len(_HZ_FIELDS)} gains, got {len(gains_seq)}"
                )
            kwargs = {name: float(gains_seq[i]) for i, (name, _) in enumerate(_HZ_FIELDS)}
        return cls(
            **kwargs,
            preamp_db=float(preamp_db),
            contributions=tuple(contributions),
        )

    @classmethod
    def flat(cls) -> "FinalEQ":
        """A no-op EQ — all bands at 0 dB, preamp 0 dB, no contributions."""
        return cls()

    # --------------------------------------------------------- serialization
    def to_dict(self, *, include_contributions: bool = False) -> dict:
        """JSON-safe dict.

        Shape::

            {
                "bands": { "31": -3.0, "62": -2.0, ..., "16000": 0.0 },
                "preamp_db": -1.5,
                "contributions": [...]   # only if include_contributions=True
            }
        """
        out: dict = {
            "bands": {str(hz): float(getattr(self, name)) for name, hz in _HZ_FIELDS},
            "preamp_db": float(self.preamp_db),
        }
        if include_contributions:
            out["contributions"] = [
                {
                    "tag_name": c.tag_name,
                    "tag_intensity": float(c.tag_intensity),
                    "preset": c.preset.as_dict(),
                    "scaled_gains": [float(g) for g in c.scaled_gains],
                }
                for c in self.contributions
            ]
        return out

    def to_json(
        self,
        path: str | Path,
        *,
        indent: int | None = 2,
        include_contributions: bool = False,
    ) -> Path:
        """Serialize to a JSON file; returns the written path."""
        out = Path(path)
        out.parent.mkdir(parents=True, exist_ok=True)
        with out.open("w", encoding="utf-8") as f:
            json.dump(
                self.to_dict(include_contributions=include_contributions),
                f,
                indent=indent,
                ensure_ascii=False,
            )
        return out

    @classmethod
    def from_dict(cls, data: dict) -> "FinalEQ":
        """Reconstruct from a :meth:`to_dict` payload.  ``contributions`` are dropped."""
        bands_map = data.get("bands", {})
        return cls.from_gains(
            gains={int(k): float(v) for k, v in bands_map.items()},
            preamp_db=float(data.get("preamp_db", 0.0)),
        )

    @classmethod
    def from_json(cls, path: str | Path) -> "FinalEQ":
        with Path(path).open("r", encoding="utf-8") as f:
            return cls.from_dict(json.load(f))


# Import-time sanity check: the class' `hz_*` fields must exactly match
# BAND_FREQUENCIES_HZ.  If someone edits one and forgets the other, we fail
# loudly at import rather than silently mis-rout a band.
_class_hz_fields = {f.name for f in fields(FinalEQ) if f.name.startswith("hz_")}
_expected_hz_fields = {name for name, _ in _HZ_FIELDS}
assert _class_hz_fields == _expected_hz_fields, (
    f"FinalEQ hz_* fields {_class_hz_fields} do not match "
    f"BAND_FREQUENCIES_HZ {_expected_hz_fields}"
)


# =============================================================================
#  Mixer — implements the formula
# =============================================================================

@dataclass(frozen=True, slots=True)
class MixerConfig:
    """Knobs for :class:`TagEQMixer`.

    Attributes:
        min_intensity: Skip tags whose intensity is below this value.
                       Default 0.0 means "include every non-zero tag" —
                       matches the literal ``Σ`` in the formula.  Raise to
                       e.g. 0.1 to ignore weak tags for performance.
        max_gain_db: Per-band positive clip.  Default +12 dB.
        min_gain_db: Per-band negative clip.  Default −12 dB.
        preamp_headroom_db: Extra margin added to the compensating preamp
                            (so ``preamp = -(max_boost + headroom)``).
    """

    min_intensity: float = 0.0
    max_gain_db: float = 12.0
    min_gain_db: float = -12.0
    preamp_headroom_db: float = 0.5


class TagEQMixer:
    """Combine per-tag :class:`EQPreset` objects into a single :class:`FinalEQ`.

    Implements the formula ``Final EQ = Σ (tag.intensity × preset.gains)`` with
    per-band summing, per-band clipping, and preamp compensation.
    """

    def __init__(
        self,
        store: PresetStore,
        config: MixerConfig | None = None,
    ) -> None:
        self._store = store
        self._config = config or MixerConfig()

    @property
    def config(self) -> MixerConfig:
        return self._config

    @property
    def store(self) -> PresetStore:
        return self._store

    # ------------------------------------------------------------------ public
    def mix(self, tags: TagSet) -> FinalEQ:
        """Turn a :class:`TagSet` into the :class:`FinalEQ`.

        Steps
        -----
            1. Initialize a per-band accumulator to 0 dB.
            2. For every tag with ``intensity >= min_intensity`` that has a
               matching preset, add ``intensity × preset.gain`` into each band.
               (Contributions to the same band are **summed**.)
            3. Clip each band to ``[min_gain_db, max_gain_db]``.
            4. Compute ``preamp_db = -(max_boost + headroom)`` if any band
               ends up boosting; otherwise 0 dB.
        """
        cfg = self._config

        # ---- Step 1: zero accumulator per band ------------------------------
        acc: dict[int, float] = {hz: 0.0 for hz in BAND_FREQUENCIES_HZ}
        contributions: list[EQContribution] = []

        # ---- Step 2: Σ (tag.intensity × preset.gains) -----------------------
        for tag in tags:
            if tag.intensity < cfg.min_intensity:
                continue
            preset = self._store.get(tag.name)
            if preset is None:
                # No preset for this tag → contributes 0 to every band.
                continue

            # Multiply preset gains by tag score, band by band.
            scaled = tuple(band.gain_db * tag.intensity for band in preset.bands)
            # Sum into the accumulator — same-band contributions accumulate.
            for hz, gain in zip(BAND_FREQUENCIES_HZ, scaled, strict=True):
                acc[hz] += gain

            contributions.append(EQContribution(
                tag_name=tag.name,
                tag_intensity=float(tag.intensity),
                preset=preset,
                scaled_gains=scaled,
            ))

        # ---- Step 3: per-band clip to [min_gain_db, max_gain_db] -----------
        for hz, gain in acc.items():
            if gain > cfg.max_gain_db:
                acc[hz] = cfg.max_gain_db
            elif gain < cfg.min_gain_db:
                acc[hz] = cfg.min_gain_db

        # ---- Step 4: preamp — only needed if any band boosts --------------
        max_boost = max((g for g in acc.values() if g > 0.0), default=0.0)
        preamp_db = (
            -(max_boost + cfg.preamp_headroom_db)
            if max_boost > 0.0
            else 0.0
        )

        return FinalEQ.from_gains(
            acc,
            preamp_db=preamp_db,
            contributions=tuple(contributions),
        )
