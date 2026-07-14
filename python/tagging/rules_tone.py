"""Tone-category tag rules — overall tonal character.

``analog`` / ``digital`` are heuristics — the FeatureVector alone can't say
whether the source went through a tape machine, so we look at correlates:
mild flatness + some low-mid warmth reads as analog; ruler-flat spectrum +
wide DR + heavy air reads as digital.

The ``neutral`` tag is renamed to ``tone_neutral`` to avoid a name clash
with ``treble_neutral``.
"""
from __future__ import annotations

from tagging.config import TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.generator import register_tag
from tagging.helpers import bell, combine_geometric, ramp_down, ramp_up
from tagging.tags import TagCategory


@register_tag("warm", TagCategory.TONE)
def warm(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Elevated low-mid + subdued presence/brilliance."""
    lows_up = ramp_up(
        d.bass_ratio + d.low_mid_ratio, cfg.get("lm_min"), cfg.get("lm_max")
    )
    highs_down = ramp_down(
        d.presence_ratio + d.brilliance_ratio, cfg.get("hi_low"), cfg.get("hi_high")
    )
    return combine_geometric(lows_up, highs_down)


@register_tag("tone_neutral", TagCategory.TONE)
def tone_neutral(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Overall spectral tilt is close to flat — no strong warm/cold bias.

    Approximated by the centroid sitting near a "flat mix" reference point.
    """
    return bell(d.centroid_hz, cfg.get("center_hz"), cfg.get("half_width_hz"))


@register_tag("cold", TagCategory.TONE)
def cold(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Elevated presence + reduced low-mid warmth."""
    highs_up = ramp_up(
        d.presence_ratio + d.brilliance_ratio, cfg.get("hi_min"), cfg.get("hi_max")
    )
    lows_down = ramp_down(
        d.bass_ratio + d.low_mid_ratio, cfg.get("lm_low"), cfg.get("lm_high")
    )
    return combine_geometric(highs_up, lows_down)


@register_tag("analog", TagCategory.TONE)
def analog(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Subtle low-mid warmth + moderate flatness (tape-hiss floor) + gentle top.

    Proxy — not a real tape-detector.  We look for a specific *combination*:
    warm low-mids, non-flat but not too spiky spectrum, and no exaggerated air.
    """
    warmth = ramp_up(
        d.bass_ratio + d.low_mid_ratio,
        cfg.get("warm_min"), cfg.get("warm_max"),
    )
    modest_flatness = bell(
        d.flatness, cfg.get("flat_center"), cfg.get("flat_half_width")
    )
    gentle_top = ramp_down(
        d.air_ratio, cfg.get("air_low"), cfg.get("air_high")
    )
    return combine_geometric(warmth, modest_flatness, gentle_top)


@register_tag("digital", TagCategory.TONE)
def digital(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Wide dynamic range + healthy air + tightly-defined transients.

    Proxy — a "clean, hi-fi" impression rather than actual digital-source
    detection.
    """
    dr_wide = ramp_up(
        d.dynamic_range_db, cfg.get("dr_min"), cfg.get("dr_max")
    )
    air_rich = ramp_up(d.air_ratio, cfg.get("air_min"), cfg.get("air_max"))
    tight = ramp_up(
        d.crest_factor_db, cfg.get("crest_min"), cfg.get("crest_max")
    )
    return combine_geometric(dr_wide, air_rich, tight)
