"""EQEngine — top-level orchestrator.

Pipeline
--------

::

    FeatureVector   ─▶   Tag Score   ─▶   EQ Preset   ─▶   Final EQ
      (features)       (tagging)         (eq.presets)     (eq.mixer)

Formula
-------

::

    Final EQ = Σ (Tag Score × Tag EQ Preset)

Every registered tag is computed (score in ``[0, 1]``); each tag with a
matching preset contributes ``score × preset_gains`` to every band; same
bands are summed; each band is finally clipped to a configurable range
(default ``±12 dB``).  A compensating preamp is added when any band ends
up boosting.

The output is a :class:`FinalEQ` dataclass with one explicit float field per
ISO octave center (``hz_31`` .. ``hz_16000``) plus ``preamp_db``.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from eq.mixer import FinalEQ, MixerConfig, TagEQMixer
from eq.presets import PresetStore
from features.extractor import FeatureVector
from tagging import TagGenerator, TagSet


@dataclass(frozen=True, slots=True)
class EQResult:
    """Bundled pipeline output for a single input file.

    Attributes:
        tags: The intensity-weighted tag set that fed the mixer.
        final_eq: The final 10-band EQ produced by summing all tag×preset
                  contributions.
    """

    tags: TagSet
    final_eq: FinalEQ


class EQEngine:
    """FeatureVector → EQResult in one call.

    Instantiate once at startup, reuse across many files — nothing on this
    class holds per-track state.

    Args:
        max_gain_db: Per-band positive clip.  Default +12 dB.
        min_gain_db: Per-band negative clip.  Default −12 dB.
        min_intensity: Skip tags whose score is below this value in the sum.
                       Default 0.0 = include every non-zero tag (the literal
                       formula).  Raise to skip weak tags for a bit of speed.
        preamp_headroom_db: Extra safety margin on the compensating preamp.
        tag_generator: Optional custom :class:`TagGenerator`; defaults to the
                       standard one with ``config/tags.yaml``.
        preset_store: Optional custom :class:`PresetStore`; defaults to the
                      standard one with ``config/eq_presets.yaml``.
        auto_reload_presets: If True and no custom ``preset_store`` is passed,
                             the default store is loaded with live-reload
                             enabled — YAML edits to ``config/eq_presets.yaml``
                             take effect on the next :meth:`compute` call.

    Example ::

        engine = EQEngine()                        # ±12 dB defaults
        result = engine.compute(feature_vector)
        result.final_eq.hz_62                      # → -1.6  (float)
        result.final_eq.as_dict()                  # → {31: -3.0, 62: -1.6, ...}
        result.final_eq.to_json("out/track.eq.json")
    """

    def __init__(
        self,
        *,
        max_gain_db: float = 12.0,
        min_gain_db: float = -12.0,
        min_intensity: float = 0.0,
        preamp_headroom_db: float = 0.5,
        tag_generator: TagGenerator | None = None,
        preset_store: PresetStore | None = None,
        auto_reload_presets: bool = False,
    ) -> None:
        self._tag_gen: TagGenerator = tag_generator or TagGenerator()
        self._store: PresetStore = preset_store or PresetStore.load_default(
            auto_reload=auto_reload_presets
        )
        self._mixer: TagEQMixer = TagEQMixer(
            store=self._store,
            config=MixerConfig(
                min_intensity=min_intensity,
                max_gain_db=max_gain_db,
                min_gain_db=min_gain_db,
                preamp_headroom_db=preamp_headroom_db,
            ),
        )

    # ---------------------------------------------------------------- accessors
    @property
    def tag_generator(self) -> TagGenerator:
        return self._tag_gen

    @property
    def preset_store(self) -> PresetStore:
        return self._store

    @property
    def mixer(self) -> TagEQMixer:
        return self._mixer

    @property
    def config(self) -> MixerConfig:
        """The active mixer configuration (clip limits, headroom, threshold)."""
        return self._mixer.config

    # ------------------------------------------------------------------ public
    def compute(self, fv: FeatureVector) -> EQResult:
        """Run the full pipeline on one :class:`FeatureVector`.

        Steps
        -----
            1. **FeatureVector → Tag Score** via :class:`TagGenerator`.
               Every registered tag is evaluated; scores fall in ``[0, 1]``.
            2. **Tag Score → EQ Preset** via :class:`PresetStore`.
               Each tag looks up its preset from YAML.
            3. **Final EQ = Σ (score × preset)** via :class:`TagEQMixer`.
               Contributions to same bands are summed; each band is clipped
               to ``[min_gain_db, max_gain_db]``; preamp is computed.
        """
        tags: TagSet = self._tag_gen.generate(fv)
        final_eq: FinalEQ = self._mixer.mix(tags)
        return EQResult(tags=tags, final_eq=final_eq)

    # --------------------------------------------------------------- factories
    @classmethod
    def with_config_paths(
        cls,
        tags_config: str | Path | None = None,
        presets_config: str | Path | None = None,
        *,
        auto_reload_presets: bool = False,
        max_gain_db: float = 12.0,
        min_gain_db: float = -12.0,
        min_intensity: float = 0.0,
    ) -> "EQEngine":
        """Build an engine from user-supplied YAML paths.

        Either config argument may be ``None`` to fall back to the shipped
        default (``config/tags.yaml``, ``config/eq_presets.yaml``).
        """
        tag_gen: TagGenerator | None = (
            TagGenerator.with_config_path(tags_config)
            if tags_config is not None
            else None
        )
        store: PresetStore | None = (
            PresetStore.load(presets_config, auto_reload=auto_reload_presets)
            if presets_config is not None
            else None
        )
        return cls(
            tag_generator=tag_gen,
            preset_store=store,
            auto_reload_presets=auto_reload_presets,
            max_gain_db=max_gain_db,
            min_gain_db=min_gain_db,
            min_intensity=min_intensity,
        )
