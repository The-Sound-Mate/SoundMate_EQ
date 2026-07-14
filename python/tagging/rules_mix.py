"""Mix-category tag rules — overall mix quality proxies.

These are heuristics; they get "muddy" and "clean" roughly right on typical
music but can be fooled by e.g. deliberately lo-fi productions.
"""
from __future__ import annotations

from tagging.config import TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.generator import register_tag
from tagging.helpers import combine_geometric, ramp_down, ramp_up
from tagging.tags import TagCategory


@register_tag("clean_mix", TagCategory.MIX)
def clean_mix(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Low low-mid buildup + healthy dynamic range + moderate flatness.

    A well-mixed track has controlled 250-500 Hz, some dynamics, and doesn't
    read as spectrally-flat (over-processed) or spiky (undermixed).
    """
    low_lowmid = ramp_down(
        d.boxy_band_ratio, cfg.get("lm_low"), cfg.get("lm_high")
    )
    dr_ok = ramp_up(
        d.dynamic_range_db, cfg.get("dr_min"), cfg.get("dr_max")
    )
    controlled_flat = ramp_down(
        d.flatness, cfg.get("flat_low"), cfg.get("flat_high")
    )
    return combine_geometric(low_lowmid, dr_ok, controlled_flat)


@register_tag("muddy_mix", TagCategory.MIX)
def muddy_mix(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Elevated 250-500 Hz relative to the full spectrum."""
    return ramp_up(
        d.boxy_band_ratio, cfg.get("ratio_min"), cfg.get("ratio_max")
    )


@register_tag("dense_mix", TagCategory.MIX)
def dense_mix(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """High onset rate + high spectral flatness — busy, wall-of-sound feel."""
    onsets = ramp_up(
        d.onset_rate_per_sec, cfg.get("onset_min"), cfg.get("onset_max")
    )
    flat = ramp_up(d.flatness, cfg.get("flat_min"), cfg.get("flat_max"))
    return combine_geometric(onsets, flat)


@register_tag("sparse_mix", TagCategory.MIX)
def sparse_mix(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Low onset rate + high spectral contrast — few elements, plenty of gaps.

    High contrast means big peak-to-valley swings in the spectrum, i.e. only
    a handful of instruments occupying distinct bands.
    """
    slow = ramp_down(
        d.onset_rate_per_sec, cfg.get("onset_low"), cfg.get("onset_high")
    )
    high_contrast = ramp_up(
        d.contrast_mean, cfg.get("contrast_min"), cfg.get("contrast_max")
    )
    return combine_geometric(slow, high_contrast)
