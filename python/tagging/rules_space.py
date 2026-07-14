"""Space-category tag rules — width, mono/stereo, wet/dry.

Width tags come straight from :class:`StereoFeatures.stereo_width`.  For
reverb/dry we use spectral flatness and crest factor as proxies (a wet mix
smears transients → lower crest, higher flatness).
"""
from __future__ import annotations

from tagging.config import TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.generator import register_tag
from tagging.helpers import bell, combine_geometric, ramp_down, ramp_up
from tagging.tags import TagCategory


# -----------------------------------------------------------------------------
#  Width — mono / narrow / wide / huge
# -----------------------------------------------------------------------------

@register_tag("mono", TagCategory.SPACE)
def mono(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """True mono file or stereo with near-zero width."""
    if not d.is_stereo:
        return 1.0
    return ramp_down(
        d.stereo_width, cfg.get("width_low"), cfg.get("width_high")
    )


@register_tag("narrow", TagCategory.SPACE)
def narrow(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Small stereo image — tent centered on a low width value."""
    return bell(d.stereo_width, cfg.get("center"), cfg.get("half_width"))


@register_tag("wide", TagCategory.SPACE)
def wide(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Broad stereo image — modern-mix wide."""
    return ramp_up(
        d.stereo_width, cfg.get("width_min"), cfg.get("width_max")
    )


@register_tag("huge", TagCategory.SPACE)
def huge(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Wide *and* de-correlated — the ambient / cinematic feel.

    ``|correlation|`` low means L and R are only weakly related, i.e. the
    ambience is doing real work rather than being a fake widener.
    """
    wide_amt = ramp_up(
        d.stereo_width, cfg.get("width_min"), cfg.get("width_max")
    )
    low_corr = ramp_down(
        abs(d.lr_correlation), cfg.get("corr_low"), cfg.get("corr_high")
    )
    return combine_geometric(wide_amt, low_corr)


# -----------------------------------------------------------------------------
#  Wet / dry
# -----------------------------------------------------------------------------

@register_tag("reverb_heavy", TagCategory.SPACE)
def reverb_heavy(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """High spectral flatness + reduced crest factor.

    A reverberant track smears transients (crest drops) and fills gaps in the
    spectrum (flatness rises).
    """
    flat = ramp_up(d.flatness, cfg.get("flat_min"), cfg.get("flat_max"))
    low_crest = ramp_down(
        d.crest_factor_db, cfg.get("crest_low"), cfg.get("crest_high")
    )
    return combine_geometric(flat, low_crest)


@register_tag("dry", TagCategory.SPACE)
def dry(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Low flatness + high crest factor — transients well defined, gaps intact."""
    low_flat = ramp_down(
        d.flatness, cfg.get("flat_low"), cfg.get("flat_high")
    )
    high_crest = ramp_up(
        d.crest_factor_db, cfg.get("crest_min"), cfg.get("crest_max")
    )
    return combine_geometric(low_flat, high_crest)
