"""Tagging layer.

Public entry point::

    from tagging import TagGenerator
    tags = TagGenerator().generate(feature_vector)
    tags.as_dict()   # {"bass_heavy": 0.82, "warm": 0.64, ...}

Layered imports
---------------
The **pure-logic** parts of this package (``Tag``, ``TagCategory``, ``TagSet``,
helper functions, YAML config loader) work without the audio stack.  The
**audio-dependent** parts (``TagGenerator``, ``DerivedFeatures``) require
librosa via :mod:`features.extractor`.  We wrap the second group in a
``try/except`` so lightweight consumers (tests, CLI-only tag inspection)
can import this package without installing the whole audio stack.
"""
from __future__ import annotations

# ----------------------------------------------------------------------------
#  Pure-logic core — safe to import without audio libs (only needs pyyaml).
# ----------------------------------------------------------------------------
from tagging.helpers import (
    bell,
    clip01,
    combine_geometric,
    combine_max,
    combine_mean,
    combine_min,
    gaussian_bell,
    ramp_down,
    ramp_up,
    sigmoid,
)
from tagging.tags import Tag, TagCategory, TagSet

# TagConfigStore only needs PyYAML at runtime; import ``yaml`` lazily inside
# the module methods so a bare Python install can still `import tagging.tags`.
from tagging.config import TagConfigStore, TagRuleConfig


# ----------------------------------------------------------------------------
#  Rule-module registry — the list is data, so it's safe to define here even
#  if the rule modules themselves can't be imported yet.
# ----------------------------------------------------------------------------

_RULE_MODULES: tuple[str, ...] = (
    "tagging.rules_bass",
    "tagging.rules_mid",
    "tagging.rules_treble",
    "tagging.rules_vocal",
    "tagging.rules_drum",
    "tagging.rules_space",
    "tagging.rules_dynamics",
    "tagging.rules_tone",
    "tagging.rules_mix",
    "tagging.rules_instrument",
)


def _load_rule_modules() -> None:
    """Import every ``rules_*`` module so their @register_tag decorators run.

    Cheap on repeated calls thanks to ``sys.modules`` caching.  Called lazily
    by :class:`TagGenerator` so we don't drag in the audio stack until the
    generator is actually instantiated.
    """
    import importlib

    for mod_name in _RULE_MODULES:
        importlib.import_module(mod_name)


# ----------------------------------------------------------------------------
#  Audio-dependent re-exports — guarded so this package still imports on a
#  minimal install.  A user who tries to *use* TagGenerator without librosa
#  will still get a clear ImportError; a user who only imports Tag / TagSet
#  is unaffected.
# ----------------------------------------------------------------------------

try:
    from tagging.derived import DerivedFeatures
    from tagging.generator import (
        GeneratorConfig,
        TagGenerator,
        register_tag,
        registered_tags,
    )

    _AUDIO_STACK_AVAILABLE: bool = True
    # Eager rule registration so ``registered_tags()`` returns the full
    # catalog immediately — but only when the imports above succeeded.
    _load_rule_modules()
except ImportError:  # pragma: no cover — depends on install
    _AUDIO_STACK_AVAILABLE = False


__all__ = [
    # Core types (always available)
    "Tag",
    "TagCategory",
    "TagSet",
    "TagConfigStore",
    "TagRuleConfig",
    # Intensity helpers (always available)
    "bell",
    "clip01",
    "combine_geometric",
    "combine_max",
    "combine_mean",
    "combine_min",
    "gaussian_bell",
    "ramp_down",
    "ramp_up",
    "sigmoid",
    # Audio-dependent (available only when librosa etc. are installed)
    "DerivedFeatures",
    "GeneratorConfig",
    "TagGenerator",
    "register_tag",
    "registered_tags",
]
