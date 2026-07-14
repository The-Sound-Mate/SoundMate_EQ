"""Drum-category tag rules — kick, snare, cymbal character.

Without stem separation we approximate each drum piece by the frequency band
it typically occupies + HPSS "percussive" energy.  These are best-effort
heuristics; they'll misfire on drumless material but are usually right on
regular songs.
"""
from __future__ import annotations

from tagging.config import TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.generator import register_tag
from tagging.helpers import bell, combine_geometric, ramp_down, ramp_up
from tagging.tags import TagCategory


# =============================================================================
#  Kick — thump (40-100 Hz) + click (2-5 kHz)
# =============================================================================

def _kick_signal(d: DerivedFeatures) -> float:
    """Combined "kick presence" proxy: thump ratio × percussive share."""
    return d.kick_thump_ratio * d.percussive_ratio


@register_tag("kick_soft", TagCategory.DRUM)
def kick_soft(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Low kick signal — either quiet drums or a hip-hop-style thumpless kick."""
    return ramp_down(_kick_signal(d), cfg.get("val_low"), cfg.get("val_high"))


@register_tag("kick_balanced", TagCategory.DRUM)
def kick_balanced(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Kick sits at the typical proportion for a well-mixed track."""
    return bell(_kick_signal(d), cfg.get("center"), cfg.get("half_width"))


@register_tag("kick_punchy", TagCategory.DRUM)
def kick_punchy(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Thump + click + hot transients — a defined, snappy kick."""
    thump = ramp_up(
        d.kick_thump_ratio, cfg.get("thump_min"), cfg.get("thump_max")
    )
    click = ramp_up(
        d.kick_click_ratio, cfg.get("click_min"), cfg.get("click_max")
    )
    crest = ramp_up(
        d.crest_factor_db, cfg.get("crest_min"), cfg.get("crest_max")
    )
    return combine_geometric(thump, click, crest)


@register_tag("kick_boomy", TagCategory.DRUM)
def kick_boomy(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Deep sub-bass + thump — a big low-end kick without click definition."""
    sub = ramp_up(
        d.sub_bass_ratio, cfg.get("sub_min"), cfg.get("sub_max")
    )
    thump = ramp_up(
        d.kick_thump_ratio, cfg.get("thump_min"), cfg.get("thump_max")
    )
    return combine_geometric(sub, thump)


# =============================================================================
#  Snare — body (150-300 Hz) + snap (4-6 kHz)
# =============================================================================

def _snare_signal(d: DerivedFeatures) -> float:
    """Combined snare-presence proxy: snap × percussive share."""
    return d.snare_snap_ratio * d.percussive_ratio


@register_tag("snare_soft", TagCategory.DRUM)
def snare_soft(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Low snap signal — brushes, ghost notes, or no snare at all."""
    return ramp_down(_snare_signal(d), cfg.get("val_low"), cfg.get("val_high"))


@register_tag("snare_balanced", TagCategory.DRUM)
def snare_balanced(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Body + snap in the typical proportion."""
    combined = d.snare_body_ratio + d.snare_snap_ratio
    return bell(combined, cfg.get("center"), cfg.get("half_width"))


@register_tag("snare_sharp", TagCategory.DRUM)
def snare_sharp(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """High snap + high onset rate — a cutting, aggressive snare."""
    snap = ramp_up(
        d.snare_snap_ratio, cfg.get("snap_min"), cfg.get("snap_max")
    )
    onset = ramp_up(
        d.onset_rate_per_sec, cfg.get("onset_min"), cfg.get("onset_max")
    )
    return combine_geometric(snap, onset)


# =============================================================================
#  Cymbal — brilliance + air (6-20 kHz)
# =============================================================================

def _cymbal_signal(d: DerivedFeatures) -> float:
    """Cymbal proxy: brilliance + air combined."""
    return d.brilliance_ratio + d.air_ratio


@register_tag("cymbal_dark", TagCategory.DRUM)
def cymbal_dark(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Low top-end — dark ride, quiet hats, or no cymbals."""
    return ramp_down(_cymbal_signal(d), cfg.get("val_low"), cfg.get("val_high"))


@register_tag("cymbal_balanced", TagCategory.DRUM)
def cymbal_balanced(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Cymbal energy sits in the typical range."""
    return bell(_cymbal_signal(d), cfg.get("center"), cfg.get("half_width"))


@register_tag("cymbal_bright", TagCategory.DRUM)
def cymbal_bright(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Strong brilliance + air — cutting, forward cymbals."""
    return ramp_up(_cymbal_signal(d), cfg.get("val_min"), cfg.get("val_max"))
