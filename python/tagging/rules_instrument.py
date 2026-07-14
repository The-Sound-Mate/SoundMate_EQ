"""Instrument-focus tag rules — best-effort dominant-instrument heuristics.

Without stem separation these use frequency-band proxies + HPSS split +
onset density.  They're less reliable than the tonal / dynamics rules;
consider adding an ML classifier later.  Each rule is deliberately
symmetric so a track can simultaneously fire e.g. ``vocal_focus`` and
``guitar_focus`` if the mix genuinely features both.
"""
from __future__ import annotations

from tagging.config import TagRuleConfig
from tagging.derived import DerivedFeatures
from tagging.generator import register_tag
from tagging.helpers import combine_geometric, ramp_down, ramp_up
from tagging.tags import TagCategory


@register_tag("vocal_focus", TagCategory.INSTRUMENT_FOCUS)
def vocal_focus(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Vocal band dominates + voiced pitch present."""
    band = ramp_up(
        d.vocal_band_ratio, cfg.get("band_min"), cfg.get("band_max")
    )
    voiced = ramp_up(
        d.voiced_ratio, cfg.get("voiced_min"), cfg.get("voiced_max")
    )
    return combine_geometric(band, voiced)


@register_tag("bass_focus", TagCategory.INSTRUMENT_FOCUS)
def bass_focus(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Bass total is the dominant band + harmonic HPSS component (real notes)."""
    bass_amt = ramp_up(
        d.bass_ratio + d.sub_bass_ratio,
        cfg.get("band_min"), cfg.get("band_max"),
    )
    harmonic = ramp_up(
        d.harmonic_ratio, cfg.get("harmonic_min"), cfg.get("harmonic_max")
    )
    return combine_geometric(bass_amt, harmonic)


@register_tag("drum_focus", TagCategory.INSTRUMENT_FOCUS)
def drum_focus(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """High percussive HPSS share + frequent onsets."""
    perc = ramp_up(
        d.percussive_ratio, cfg.get("perc_min"), cfg.get("perc_max")
    )
    onsets = ramp_up(
        d.onset_rate_per_sec, cfg.get("onset_min"), cfg.get("onset_max")
    )
    return combine_geometric(perc, onsets)


@register_tag("piano_focus", TagCategory.INSTRUMENT_FOCUS)
def piano_focus(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Broad mid-range coverage + harmonic-dominant + moderate onset rate.

    Piano covers 250 Hz – 4 kHz, is strongly harmonic (HPSS), and has a
    characteristic per-note attack rate.
    """
    range_ok = ramp_up(
        d.piano_range_ratio, cfg.get("range_min"), cfg.get("range_max")
    )
    harmonic = ramp_up(
        d.harmonic_ratio, cfg.get("harmonic_min"), cfg.get("harmonic_max")
    )
    onsets = ramp_up(
        d.onset_rate_per_sec, cfg.get("onset_min"), cfg.get("onset_max")
    )
    return combine_geometric(range_ok, harmonic, onsets)


@register_tag("guitar_focus", TagCategory.INSTRUMENT_FOCUS)
def guitar_focus(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Emphasis in 100 Hz – 2 kHz + harmonic dominance + moderate onset."""
    range_ok = ramp_up(
        d.guitar_range_ratio, cfg.get("range_min"), cfg.get("range_max")
    )
    harmonic = ramp_up(
        d.harmonic_ratio, cfg.get("harmonic_min"), cfg.get("harmonic_max")
    )
    return combine_geometric(range_ok, harmonic)


@register_tag("synth_focus", TagCategory.INSTRUMENT_FOCUS)
def synth_focus(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Strong 3-12 kHz band + high harmonic ratio + high flatness (rich texture).

    Synths tend to have more spectrally-flat, sustained content than acoustic
    instruments.  Flatness alone would misfire on noise/reverb; combining with
    harmonic ratio and a mid-treble emphasis narrows it down.
    """
    band = ramp_up(
        d.synth_hf_ratio, cfg.get("band_min"), cfg.get("band_max")
    )
    harmonic = ramp_up(
        d.harmonic_ratio, cfg.get("harmonic_min"), cfg.get("harmonic_max")
    )
    flat = ramp_up(d.flatness, cfg.get("flat_min"), cfg.get("flat_max"))
    return combine_geometric(band, harmonic, flat)


@register_tag("strings_focus", TagCategory.INSTRUMENT_FOCUS)
def strings_focus(d: DerivedFeatures, cfg: TagRuleConfig) -> float:
    """Sustained mid content + harmonic-dominant + low onset rate.

    Bowed strings are the archetypal "sustained + harmonic + few attacks"
    combination — that's exactly what we look for.
    """
    range_ok = ramp_up(
        d.strings_range_ratio, cfg.get("range_min"), cfg.get("range_max")
    )
    harmonic = ramp_up(
        d.harmonic_ratio, cfg.get("harmonic_min"), cfg.get("harmonic_max")
    )
    slow = ramp_down(
        d.onset_rate_per_sec, cfg.get("onset_low"), cfg.get("onset_high")
    )
    return combine_geometric(range_ok, harmonic, slow)
