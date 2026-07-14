"""Bass-category tag rules.

All rules read pre-computed band-energy ratios out of :class:`DerivedFeatures`
so nothing has to touch the raw FFT.  The bass total is defined as
``sub_bass_ratio + bass_ratio`` (20 – 250 Hz combined).
"""
from __future__ import annotations

from tagging.config import TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.generator import register_tag
from tagging.helpers import bell, combine_geometric, ramp_down, ramp_up
from tagging.tags import TagCategory


def _bass_total(d: DerivedFeatures) -> float:
    """Combined sub-bass + bass energy fraction (20 – 250 Hz)."""
    return d.sub_bass_ratio + d.bass_ratio


# -----------------------------------------------------------------------------
#  Amount of bass — mutually-graded rules over `_bass_total(d)`
# -----------------------------------------------------------------------------

@register_tag("bass_none", TagCategory.BASS)
def bass_none(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Almost no low-end at all — the track lives above 250 Hz."""
    return ramp_down(_bass_total(d), cfg.get("ratio_low"), cfg.get("ratio_high"))


@register_tag("bass_light", TagCategory.BASS)
def bass_light(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Present but restrained bass — a tent centered on a low ratio."""
    return bell(_bass_total(d), cfg.get("center"), cfg.get("half_width"))


@register_tag("bass_balanced", TagCategory.BASS)
def bass_balanced(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Bass in the sweet spot — tent centered around a typical mixed track."""
    return bell(_bass_total(d), cfg.get("center"), cfg.get("half_width"))


@register_tag("bass_heavy", TagCategory.BASS)
def bass_heavy(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Elevated bass — clearly more low-end than a neutral mix."""
    return ramp_up(_bass_total(d), cfg.get("ratio_min"), cfg.get("ratio_max"))


@register_tag("bass_overpower", TagCategory.BASS)
def bass_overpower(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Bass dominates the spectrum — starts higher than ``bass_heavy``."""
    return ramp_up(_bass_total(d), cfg.get("ratio_min"), cfg.get("ratio_max"))


# -----------------------------------------------------------------------------
#  Character of the bass — sub / punch / mud / tightness
# -----------------------------------------------------------------------------

@register_tag("sub_bass", TagCategory.BASS)
def sub_bass(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Significant energy specifically below 60 Hz."""
    return ramp_up(d.sub_bass_ratio, cfg.get("ratio_min"), cfg.get("ratio_max"))


@register_tag("punchy_bass", TagCategory.BASS)
def punchy_bass(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Bass present + strong transients + generous crest factor.

    Geometric mean — a rule where every factor must be non-trivial to fire.
    """
    bass_amt = ramp_up(_bass_total(d), cfg.get("bass_min"), cfg.get("bass_max"))
    percussive = ramp_up(d.percussive_ratio, cfg.get("perc_min"), cfg.get("perc_max"))
    crest = ramp_up(d.crest_factor_db, cfg.get("crest_min"), cfg.get("crest_max"))
    return combine_geometric(bass_amt, percussive, crest)


@register_tag("muddy_bass", TagCategory.BASS)
def muddy_bass(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Low-mid energy (250–500 Hz) elevated *relative to* the bass proper.

    A high ratio here means the low-mids are drowning the bass definition.
    """
    denom = _bass_total(d) + 1e-6
    ratio = d.low_mid_ratio / denom
    return ramp_up(ratio, cfg.get("ratio_min"), cfg.get("ratio_max"))


@register_tag("tight_bass", TagCategory.BASS)
def tight_bass(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Well-defined bass — audible + dynamic + percussive-dominant."""
    bass_amt = ramp_up(_bass_total(d), cfg.get("bass_min"), cfg.get("bass_max"))
    dynamic = ramp_up(d.dynamic_range_db, cfg.get("dr_min"), cfg.get("dr_max"))
    perc = ramp_up(d.percussive_ratio, cfg.get("perc_min"), cfg.get("perc_max"))
    return combine_geometric(bass_amt, dynamic, perc)
