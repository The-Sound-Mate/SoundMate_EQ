"""YAML-backed configuration for tag rules.

Every rule function reads its numeric thresholds from a :class:`TagRuleConfig`
that comes from ``config/tags.yaml``.  The schema per tag is::

    <tag_name>:
        enabled: true          # optional, default True
        weight:  1.0           # optional, default 1.0 — post-multiplied on the intensity
        thresholds:            # rule-specific numeric constants
            centroid_min_hz: 2500
            centroid_max_hz: 4500

Missing thresholds raise :class:`KeyError` at compute time so a typo in YAML
fails loudly rather than silently returning garbage.

Missing tags (present in the registry but not in YAML) are treated as
``enabled=False`` — this is how a user disables a whole category by deletion.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml


@dataclass(frozen=True, slots=True)
class TagRuleConfig:
    """Configuration for one tag rule.

    Attributes:
        name: The tag name this config applies to.
        enabled: If False, :class:`TagGenerator` skips the rule entirely.
        weight: Post-multiplier applied to the computed intensity — a way to
                globally strengthen / weaken a tag without changing thresholds.
                Result is still clipped to ``[0, 1]``.
        thresholds: Free-form numeric constants keyed by string names the rule
                    function looks up via :meth:`get`.
    """

    name: str
    enabled: bool = True
    weight: float = 1.0
    thresholds: dict[str, float] = field(default_factory=dict)

    def get(self, key: str) -> float:
        """Fetch a threshold value, raising a clear error if it's missing."""
        if key not in self.thresholds:
            raise KeyError(
                f"tag '{self.name}': threshold '{key}' not defined in config"
            )
        return float(self.thresholds[key])

    def get_or(self, key: str, default: float) -> float:
        """Fetch a threshold with a fallback default — for rarely-set knobs."""
        return float(self.thresholds.get(key, default))


class TagConfigStore:
    """In-memory store of :class:`TagRuleConfig` keyed by tag name.

    Instances are produced by :meth:`load` (from YAML) or :meth:`from_dict`
    (from an already-parsed dict — handy for tests).
    """

    def __init__(self, configs: dict[str, TagRuleConfig]) -> None:
        self._configs: dict[str, TagRuleConfig] = dict(configs)

    # --------------------------------------------------------------- accessors
    def get(self, name: str) -> TagRuleConfig:
        """Return the config for ``name``; unknown tags come back disabled.

        Returning a disabled placeholder (rather than raising) makes the
        generator resilient to configs that leave some tags out — those tags
        are simply not produced.
        """
        cfg = self._configs.get(name)
        if cfg is None:
            return TagRuleConfig(name=name, enabled=False, weight=1.0, thresholds={})
        return cfg

    def has(self, name: str) -> bool:
        return name in self._configs

    def names(self) -> tuple[str, ...]:
        return tuple(self._configs.keys())

    def __len__(self) -> int:
        return len(self._configs)

    # --------------------------------------------------------------- factories
    @classmethod
    def load(cls, path: str | Path) -> "TagConfigStore":
        """Load ``config/tags.yaml`` (or any user-supplied path)."""
        with Path(path).open("r", encoding="utf-8") as f:
            raw: dict[str, Any] = yaml.safe_load(f) or {}
        return cls.from_dict(raw)

    @classmethod
    def from_dict(cls, raw: dict[str, Any]) -> "TagConfigStore":
        """Build a store from an already-parsed dict (YAML content or literal)."""
        tags_block: dict[str, Any] = raw.get("tags", {}) or {}
        configs: dict[str, TagRuleConfig] = {}
        for name, spec in tags_block.items():
            if not isinstance(spec, dict):
                # Skip malformed entries rather than fail hard on load.
                continue
            configs[name] = TagRuleConfig(
                name=name,
                enabled=bool(spec.get("enabled", True)),
                weight=float(spec.get("weight", 1.0)),
                thresholds={k: float(v) for k, v in (spec.get("thresholds") or {}).items()},
            )
        return cls(configs)

    @classmethod
    def default_path(cls) -> Path:
        """Path to the shipped ``config/tags.yaml``."""
        # <project>/tagging/config.py → <project>/config/tags.yaml
        return Path(__file__).resolve().parent.parent / "config" / "tags.yaml"

    @classmethod
    def load_default(cls) -> "TagConfigStore":
        """Convenience: load the shipped default config."""
        return cls.load(cls.default_path())
