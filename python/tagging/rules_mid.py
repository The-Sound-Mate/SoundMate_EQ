"""Mid-category tag rules.

"Mid" refers to the 500 – 2000 Hz band that carries most instrument body and
vocal fundamentals.  Character tags (``boxy``, ``nasal``, ``honky``) use
narrower sub-ranges that overlap the primary mid band.
"""
from __future__ import annotations

from tagging.config import TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.generator import register_tag
from tagging.helpers import bell, combine_geometric, ramp_down, ramp_up
from tagging.tags import TagCategory


# -----------------------------------------------------------------------------
#  Amount of mid — recessed / balanced / forward
# -----------------------------------------------------------------------------

@register_tag("mid_recessed", TagCategory.MID)
def mid_recessed(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Scooped mids — the "smiley EQ" shape common in metal / EDM."""
    return ramp_down(d.mid_ratio, cfg.get("ratio_low"), cfg.get("ratio_high"))


@register_tag("mid_balanced", TagCategory.MID)
def mid_balanced(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Mids sit in the typical range for a well-mixed track."""
    return bell(d.mid_ratio, cfg.get("center"), cfg.get("half_width"))


@register_tag("mid_forward", TagCategory.MID)
def mid_forward(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Mid-heavy mix — vocal / guitar / horn territory pushed forward."""
    return ramp_up(d.mid_ratio, cfg.get("ratio_min"), cfg.get("ratio_max"))


# -----------------------------------------------------------------------------
#  Mid character — boxy / nasal / honky
# -----------------------------------------------------------------------------

@register_tag("boxy", TagCategory.MID)
def boxy(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Excess energy in 250 – 500 Hz — cardboard-box coloration."""
    return ramp_up(d.boxy_band_ratio, cfg.get("ratio_min"), cfg.get("ratio_max"))


@register_tag("nasal", TagCategory.MID)
def nasal(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Excess energy in 800 – 1500 Hz — vowel-like coloration."""
    return ramp_up(d.nasal_band_ratio, cfg.get("ratio_min"), cfg.get("ratio_max"))


@register_tag("honky", TagCategory.MID)
def honky(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Peaky 500 – 1000 Hz over a narrow bandwidth.

    Both conditions must be present — honk needs the energy *and* the peak
    shape (a wideband mid boost reads as ``mid_forward`` instead).
    """
    energy = ramp_up(d.honky_band_ratio, cfg.get("ratio_min"), cfg.get("ratio_max"))
    narrow = ramp_down(d.bandwidth_hz, cfg.get("bw_narrow_hz"), cfg.get("bw_wide_hz"))
    return combine_geometric(energy, narrow)
