"""Tag registry + :class:`TagGenerator` orchestrator.

Rules register themselves via the :func:`register_tag` decorator at import
time.  :class:`TagGenerator` walks the registry once per input, feeding each
rule its :class:`TagRuleConfig` and the pre-computed :class:`DerivedFeatures`,
and packages the results into a :class:`TagSet`.

Import model
------------
Rule modules must be imported before :meth:`TagGenerator.generate` is called
or the registry will be empty.  ``tagging/__init__.py`` handles this — it
imports every ``rules_*`` module so callers get a fully populated registry
just by writing ``from tagging import TagGenerator``.
"""
from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from features.extractor import FeatureVector

from tagging.config import TagConfigStore, TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.helpers import clip01
from tagging.tags import Tag, TagCategory, TagSet


# =============================================================================
#  Registry
# =============================================================================

# A rule takes derived features + its own config and returns an intensity in
# [0, 1].  Anything outside that range is clipped by the generator.
RuleFn = Callable[[DerivedFeatures, TagRuleConfig], float]


# Module-level registry: name → (category, rule function).
# Rule modules populate this at import time via ``@register_tag``.
_REGISTRY: dict[str, tuple[TagCategory, RuleFn]] = {}


def register_tag(name: str, category: TagCategory) -> Callable[[RuleFn], RuleFn]:
    """Decorator: attach a rule function to ``name`` under ``category``.

    Duplicate names raise :class:`ValueError` at import time so accidental
    shadowing (e.g. two ``neutral`` tags) fails fast rather than silently.
    """
    def decorator(fn: RuleFn) -> RuleFn:
        if name in _REGISTRY:
            existing_cat, _ = _REGISTRY[name]
            raise ValueError(
                f"Duplicate tag name '{name}' (already registered under "
                f"{existing_cat.value}; re-registering under {category.value})"
            )
        _REGISTRY[name] = (category, fn)
        return fn
    return decorator


def registered_tags() -> tuple[tuple[str, TagCategory], ...]:
    """Snapshot of ``(name, category)`` for every registered rule.  For tests."""
    return tuple((name, cat) for name, (cat, _) in _REGISTRY.items())


# =============================================================================
#  Generator
# =============================================================================

@dataclass(frozen=True, slots=True)
class GeneratorConfig:
    """Post-processing knobs applied after every rule has run.

    Attributes:
        min_intensity: Tags whose final intensity is below this threshold are
                       dropped from the returned :class:`TagSet`.  0.0 keeps
                       everything (including 0-intensity tags).
        clip_intensity: If True (default), rule outputs are clamped to
                        ``[0, 1]`` before ``weight`` is applied, and again
                        after.  Turn off only for debugging.
    """

    min_intensity: float = 0.0
    clip_intensity: bool = True


class TagGenerator:
    """Turn a :class:`FeatureVector` into an intensity-weighted :class:`TagSet`.

    Args:
        config_store: Threshold / weight config for every tag.  If ``None``
                      (default) the shipped ``config/tags.yaml`` is loaded.
        generator_config: Post-processing options (see :class:`GeneratorConfig`).

    Example ::

        gen = TagGenerator()
        tags = gen.generate(feature_vector)
        print(tags.as_dict())
        # → {"bass_heavy": 0.82, "warm": 0.64, ...}
    """

    def __init__(
        self,
        config_store: TagConfigStore | None = None,
        generator_config: GeneratorConfig | None = None,
    ) -> None:
        # Importing rule modules populates ``_REGISTRY`` as a side effect.
        # The import lives inside ``__init__`` so a user who imports
        # ``TagGenerator`` from ``tagging`` triggers registration even if
        # ``tagging/__init__.py`` hasn't done it yet (e.g. in stripped-down
        # test environments).
        from tagging import _load_rule_modules  # noqa: PLC0415 (deferred)
        _load_rule_modules()

        self._config: TagConfigStore = config_store or TagConfigStore.load_default()
        self._opts: GeneratorConfig = generator_config or GeneratorConfig()

    # -------------------------------------------------------------- properties
    @property
    def config(self) -> TagConfigStore:
        return self._config

    # ------------------------------------------------------------------ public
    def generate(self, fv: FeatureVector) -> TagSet:
        """Run every enabled rule and collect the results."""
        derived = DerivedFeatures.from_feature_vector(fv)

        tags: list[Tag] = []
        for name, (category, rule_fn) in _REGISTRY.items():
            cfg = self._config.get(name)
            if not cfg.enabled:
                continue

            intensity = self._run_one(rule_fn, derived, cfg)
            if intensity < self._opts.min_intensity:
                continue

            tags.append(Tag(name=name, category=category, intensity=intensity))

        return TagSet(tags=tuple(tags))

    # ---------------------------------------------------------------- private
    def _run_one(
        self,
        rule_fn: RuleFn,
        derived: DerivedFeatures,
        cfg: TagRuleConfig,
    ) -> float:
        """Execute a single rule with the safety clamps applied."""
        try:
            raw = float(rule_fn(derived, cfg))
        except KeyError:
            # A threshold this rule expects is missing from YAML — propagate
            # so misconfiguration surfaces during testing.
            raise
        except Exception as exc:  # broad — an individual bad rule shouldn't kill the batch
            raise RuntimeError(
                f"tag '{cfg.name}' rule crashed: {type(exc).__name__}: {exc}"
            ) from exc

        if self._opts.clip_intensity:
            raw = clip01(raw)
        weighted = raw * cfg.weight
        if self._opts.clip_intensity:
            weighted = clip01(weighted)
        return weighted

    # --------------------------------------------------------------- factories
    @classmethod
    def with_config_path(cls, path: str | Path) -> "TagGenerator":
        """Build a generator from a user-supplied YAML path."""
        return cls(config_store=TagConfigStore.load(path))
