"""Dynamics-category tag rules — driven by dynamic range + onset density."""
from __future__ import annotations

from tagging.config import TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.generator import register_tag
from tagging.helpers import bell, combine_geometric, ramp_down, ramp_up
from tagging.tags import TagCategory


@register_tag("compressed", TagCategory.DYNAMICS)
def compressed(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Low dynamic range — heavily limited / brick-walled masters."""
    return ramp_down(
        d.dynamic_range_db, cfg.get("dr_low"), cfg.get("dr_high")
    )


@register_tag("balanced", TagCategory.DYNAMICS)
def balanced(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Dynamic range in the sweet spot — modern loudness with breathing room."""
    return bell(
        d.dynamic_range_db, cfg.get("center_db"), cfg.get("half_width_db")
    )


@register_tag("dynamic", TagCategory.DYNAMICS)
def dynamic(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Wide dynamic range — classical, jazz, careful mastering."""
    return ramp_up(
        d.dynamic_range_db, cfg.get("dr_min"), cfg.get("dr_max")
    )


@register_tag("transient_heavy", TagCategory.DYNAMICS)
def transient_heavy(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Many onsets per second + high crest factor — busy, transient-driven mix."""
    onsets = ramp_up(
        d.onset_rate_per_sec, cfg.get("onset_min"), cfg.get("onset_max")
    )
    crest = ramp_up(
        d.crest_factor_db, cfg.get("crest_min"), cfg.get("crest_max")
    )
    return combine_geometric(onsets, crest)
