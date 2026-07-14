"""Vocal-category tag rules.

The primary signal is ``vocal_band_ratio`` (300 – 3400 Hz — the classic
telephony / speech-intelligibility range) combined with :class:`PitchFeatures`'
``voiced_ratio`` (fraction of frames with detected pitch).  Voiced ratio is
the closest thing we have to a "vocal presence" flag without a full source
separation model.
"""
from __future__ import annotations

from tagging.config import TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.generator import register_tag
from tagging.helpers import bell, combine_geometric, ramp_down, ramp_up
from tagging.tags import TagCategory


# -----------------------------------------------------------------------------
#  How present is the vocal?
# -----------------------------------------------------------------------------

@register_tag("vocal_buried", TagCategory.VOCAL)
def vocal_buried(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Little energy in the vocal band relative to the rest of the mix."""
    return ramp_down(
        d.vocal_band_ratio, cfg.get("ratio_low"), cfg.get("ratio_high")
    )


@register_tag("vocal_balanced", TagCategory.VOCAL)
def vocal_balanced(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Vocal band sits at the level of a well-balanced mix."""
    return bell(d.vocal_band_ratio, cfg.get("center"), cfg.get("half_width"))


@register_tag("vocal_forward", TagCategory.VOCAL)
def vocal_forward(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Vocal band pushed forward — dominant midrange energy."""
    return ramp_up(
        d.vocal_band_ratio, cfg.get("ratio_min"), cfg.get("ratio_max")
    )


# -----------------------------------------------------------------------------
#  Vocal character
# -----------------------------------------------------------------------------

@register_tag("vocal_sibilant", TagCategory.VOCAL)
def vocal_sibilant(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Excess energy in 5 – 9 kHz — the "S" and "T" consonant range."""
    return ramp_up(
        d.sibilance_ratio, cfg.get("ratio_min"), cfg.get("ratio_max")
    )


@register_tag("breathy", TagCategory.VOCAL)
def breathy(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """High air-band energy + measurable voiced content.

    Air alone is just brightness; combined with voiced pitch it reads as
    breathy character.
    """
    air = ramp_up(d.air_ratio, cfg.get("air_min"), cfg.get("air_max"))
    voiced = ramp_up(
        d.voiced_ratio, cfg.get("voiced_min"), cfg.get("voiced_max")
    )
    return combine_geometric(air, voiced)


@register_tag("intimate", TagCategory.VOCAL)
def intimate(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Compressed dynamics + prominent mids + voiced pitch → close-mic feel."""
    compressed = ramp_down(
        d.dynamic_range_db, cfg.get("dr_low"), cfg.get("dr_high")
    )
    mid_present = ramp_up(d.mid_ratio, cfg.get("mid_min"), cfg.get("mid_max"))
    voiced = ramp_up(
        d.voiced_ratio, cfg.get("voiced_min"), cfg.get("voiced_max")
    )
    return combine_geometric(compressed, mid_present, voiced)


@register_tag("powerful_vocal", TagCategory.VOCAL)
def powerful_vocal(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Loud mids + hot peaks + strong voiced content."""
    mid = ramp_up(d.mid_ratio, cfg.get("mid_min"), cfg.get("mid_max"))
    peak = ramp_up(
        d.peak_dbfs, cfg.get("peak_min_dbfs"), cfg.get("peak_max_dbfs")
    )
    voiced = ramp_up(
        d.voiced_ratio, cfg.get("voiced_min"), cfg.get("voiced_max")
    )
    return combine_geometric(mid, peak, voiced)
