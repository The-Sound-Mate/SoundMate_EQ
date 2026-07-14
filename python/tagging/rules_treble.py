"""Treble-category tag rules.

Treble spans presence (4-6 kHz), brilliance (6-12 kHz) and air (12-20 kHz).
The primary knob for most rules is ``centroid_hz`` — the spectrum's centre
of mass — supplemented by the per-band ratios for finer character tags.

Note the tag ``treble_neutral`` (not ``neutral``) — the tone category also
has a "neutral" concept and dict keys must be unique.  See ``tone.neutral`` →
``tone_neutral``.
"""
from __future__ import annotations

from tagging.config import TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.generator import register_tag
from tagging.helpers import bell, combine_geometric, ramp_down, ramp_up
from tagging.tags import TagCategory


# -----------------------------------------------------------------------------
#  Amount of treble — dark / neutral / bright / airy
# -----------------------------------------------------------------------------

@register_tag("dark", TagCategory.TREBLE)
def dark(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Low centroid + subdued top-end bands."""
    low_centroid = ramp_down(
        d.centroid_hz, cfg.get("centroid_low_hz"), cfg.get("centroid_high_hz")
    )
    low_top = ramp_down(
        d.brilliance_ratio + d.air_ratio, cfg.get("top_low"), cfg.get("top_high")
    )
    return combine_geometric(low_centroid, low_top)


@register_tag("treble_neutral", TagCategory.TREBLE)
def treble_neutral(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Centroid sits in the typical range — neither dark nor bright.

    Renamed from spec's ``neutral`` to disambiguate from ``tone_neutral``.
    """
    return bell(d.centroid_hz, cfg.get("center_hz"), cfg.get("half_width_hz"))


@register_tag("bright", TagCategory.TREBLE)
def bright(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """High centroid + healthy presence / brilliance bands."""
    high_centroid = ramp_up(
        d.centroid_hz, cfg.get("centroid_min_hz"), cfg.get("centroid_max_hz")
    )
    high_top = ramp_up(
        d.presence_ratio + d.brilliance_ratio, cfg.get("top_min"), cfg.get("top_max")
    )
    return combine_geometric(high_centroid, high_top)


@register_tag("airy", TagCategory.TREBLE)
def airy(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Extension above 12 kHz — the "air band"."""
    return ramp_up(d.air_ratio, cfg.get("ratio_min"), cfg.get("ratio_max"))


# -----------------------------------------------------------------------------
#  Character — harsh / smooth / crisp / sparkly
# -----------------------------------------------------------------------------

@register_tag("harsh", TagCategory.TREBLE)
def harsh(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Peaky presence band + hot peaks — the "in-your-face" combination."""
    presence = ramp_up(
        d.presence_ratio, cfg.get("presence_min"), cfg.get("presence_max")
    )
    peak_hot = ramp_up(
        d.peak_dbfs, cfg.get("peak_min_dbfs"), cfg.get("peak_max_dbfs")
    )
    return combine_geometric(presence, peak_hot)


@register_tag("smooth", TagCategory.TREBLE)
def smooth(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Low spectral flatness — a smooth, non-peaky top-end response."""
    return ramp_down(d.flatness, cfg.get("flat_low"), cfg.get("flat_high"))


@register_tag("crisp", TagCategory.TREBLE)
def crisp(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Frequent onsets + energy in high-mid / presence bands.

    Crispness is about **transient clarity in the upper mids**, not raw air.
    """
    onsets = ramp_up(
        d.onset_rate_per_sec, cfg.get("onset_min"), cfg.get("onset_max")
    )
    hi_mids = ramp_up(
        d.high_mid_ratio + d.presence_ratio, cfg.get("hi_min"), cfg.get("hi_max")
    )
    return combine_geometric(onsets, hi_mids)


@register_tag("sparkly", TagCategory.TREBLE)
def sparkly(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Air + percussive character — the shimmering high-end of a good cymbal."""
    air = ramp_up(d.air_ratio, cfg.get("air_min"), cfg.get("air_max"))
    perc = ramp_up(
        d.percussive_ratio, cfg.get("perc_min"), cfg.get("perc_max")
    )
    return combine_geometric(air, perc)
