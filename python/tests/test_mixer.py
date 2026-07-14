"""TagEQMixer — verify the formula ``Final EQ = Σ (score × preset)``.

These tests do not need audio libs; they feed the mixer synthetic TagSets
and PresetStores and inspect the numeric output.
"""
from __future__ import annotations

import pytest

from eq.mixer import EQContribution, FinalEQ, MixerConfig, TagEQMixer
from eq.presets import EQPreset, PresetStore
from tagging.tags import Tag, TagCategory, TagSet


def _store(**presets: dict[int, float]) -> PresetStore:
    """Build an in-memory PresetStore from ``{tag_name: {freq: gain}}``."""
    return PresetStore({
        name: EQPreset.from_gains(name, gains) for name, gains in presets.items()
    })


def _tags(**intensities: float) -> TagSet:
    """Build a TagSet from ``{name: intensity}`` (all under BASS for simplicity)."""
    return TagSet(tags=tuple(
        Tag(name=name, category=TagCategory.BASS, intensity=intensity)
        for name, intensity in intensities.items()
    ))


# =============================================================================
#  Formula: Final EQ = Σ (score × preset)
# =============================================================================

class TestFormula:
    def test_single_tag_scales_preset(self):
        store = _store(bass_heavy={31: -3.0, 62: -2.0, 125: -1.0})
        tags = _tags(bass_heavy=0.5)
        eq = TagEQMixer(store).mix(tags)

        # score=0.5 → gains halved
        assert eq.hz_31 == pytest.approx(-1.5)
        assert eq.hz_62 == pytest.approx(-1.0)
        assert eq.hz_125 == pytest.approx(-0.5)
        # Unlisted bands stay at 0
        assert eq.hz_250 == 0.0
        assert eq.hz_16000 == 0.0

    def test_full_intensity_matches_preset(self):
        store = _store(bass_heavy={31: -3.0, 62: -2.0})
        eq = TagEQMixer(store).mix(_tags(bass_heavy=1.0))
        assert eq.hz_31 == -3.0
        assert eq.hz_62 == -2.0

    def test_zero_intensity_contributes_nothing(self):
        store = _store(bass_heavy={31: -3.0})
        eq = TagEQMixer(store).mix(_tags(bass_heavy=0.0))
        assert eq.is_flat()

    def test_same_band_contributions_are_summed(self):
        # Two different tags both cutting 62 Hz → total is the sum.
        store = _store(
            bass_heavy={62: -2.0},
            muddy_mix={62: -1.0},
        )
        tags = _tags(bass_heavy=1.0, muddy_mix=1.0)
        eq = TagEQMixer(store).mix(tags)
        assert eq.hz_62 == pytest.approx(-3.0)

    def test_missing_preset_contributes_zero(self):
        store = _store(bass_heavy={31: -3.0})
        # 'warm' has no preset in the store — the mixer just skips it.
        tags = _tags(bass_heavy=0.5, warm=0.9)
        eq = TagEQMixer(store).mix(tags)
        assert eq.hz_31 == pytest.approx(-1.5)

    def test_user_example_end_to_end(self):
        """Reproduce the user's example numerically.

        bass_heavy@0.82, bright@0.31, vocal_forward@0.5
        """
        store = _store(
            bass_heavy={31: -3, 62: -2, 125: -1},
            bright={4000: -2, 8000: -2},
            vocal_forward={1000: -1, 2000: -2, 4000: -1},
        )
        tags = TagSet(tags=(
            Tag("bass_heavy",    TagCategory.BASS,   0.82),
            Tag("bright",        TagCategory.TREBLE, 0.31),
            Tag("vocal_forward", TagCategory.VOCAL,  0.50),
        ))
        eq = TagEQMixer(store).mix(tags)

        assert eq.hz_31 == pytest.approx(-3 * 0.82)
        assert eq.hz_62 == pytest.approx(-2 * 0.82)
        assert eq.hz_125 == pytest.approx(-1 * 0.82)
        assert eq.hz_1000 == pytest.approx(-1 * 0.50)
        assert eq.hz_2000 == pytest.approx(-2 * 0.50)
        # 4 kHz gets contributions from BOTH bright and vocal_forward.
        assert eq.hz_4000 == pytest.approx(-2 * 0.31 + -1 * 0.50)
        assert eq.hz_8000 == pytest.approx(-2 * 0.31)
        # Unlisted stays flat.
        assert eq.hz_16000 == 0.0


# =============================================================================
#  Per-band clipping (default ±12 dB)
# =============================================================================

class TestClipping:
    def test_positive_clip(self):
        # Boost of +20 dB × 1.0 → clipped to +12 dB by default.
        store = _store(bass_none={31: 20.0})
        eq = TagEQMixer(store).mix(_tags(bass_none=1.0))
        assert eq.hz_31 == 12.0

    def test_negative_clip(self):
        store = _store(bass_heavy={31: -20.0})
        eq = TagEQMixer(store).mix(_tags(bass_heavy=1.0))
        assert eq.hz_31 == -12.0

    def test_custom_clip_range(self):
        store = _store(bass_heavy={31: -10.0})
        config = MixerConfig(max_gain_db=6.0, min_gain_db=-6.0)
        eq = TagEQMixer(store, config).mix(_tags(bass_heavy=1.0))
        assert eq.hz_31 == -6.0

    def test_clip_is_per_band(self):
        """Each band is clipped independently — one runaway band doesn't
        affect its neighbors."""
        store = _store(x={31: -20.0, 62: -3.0})
        eq = TagEQMixer(store).mix(_tags(x=1.0))
        assert eq.hz_31 == -12.0  # clipped
        assert eq.hz_62 == -3.0    # untouched


# =============================================================================
#  Preamp compensation
# =============================================================================

class TestPreamp:
    def test_zero_preamp_when_only_cutting(self):
        store = _store(bass_heavy={31: -3.0, 62: -2.0})
        eq = TagEQMixer(store).mix(_tags(bass_heavy=1.0))
        assert eq.preamp_db == 0.0

    def test_negative_preamp_when_boosting(self):
        store = _store(bass_none={62: 4.0})
        config = MixerConfig(preamp_headroom_db=0.5)
        eq = TagEQMixer(store, config).mix(_tags(bass_none=1.0))
        # preamp = -(4.0 + 0.5) = -4.5
        assert eq.preamp_db == pytest.approx(-4.5)

    def test_preamp_uses_max_across_bands(self):
        store = _store(x={62: 2.0, 4000: 5.0})
        eq = TagEQMixer(store, MixerConfig(preamp_headroom_db=0.0)).mix(_tags(x=1.0))
        # Highest boost is 5 dB at 4 kHz → preamp = -5.
        assert eq.preamp_db == pytest.approx(-5.0)


# =============================================================================
#  min_intensity filtering
# =============================================================================

class TestMinIntensity:
    def test_zero_default_includes_everything(self):
        # Formula-literal default: even 0.001-intensity tags contribute.
        store = _store(x={31: -10.0})
        eq = TagEQMixer(store).mix(_tags(x=0.01))
        assert eq.hz_31 == pytest.approx(-0.1)

    def test_threshold_skips_weak_tags(self):
        store = _store(x={31: -10.0})
        config = MixerConfig(min_intensity=0.5)
        eq = TagEQMixer(store, config).mix(_tags(x=0.3))
        assert eq.is_flat()


# =============================================================================
#  Contribution trace
# =============================================================================

class TestContributions:
    def test_records_every_active_tag(self):
        store = _store(
            bass_heavy={31: -3.0},
            bright={4000: -2.0},
        )
        tags = _tags(bass_heavy=0.5, bright=0.4)
        eq = TagEQMixer(store).mix(tags)

        assert len(eq.contributions) == 2
        names = {c.tag_name for c in eq.contributions}
        assert names == {"bass_heavy", "bright"}

    def test_scaled_gains_match_intensity_times_preset(self):
        store = _store(bass_heavy={31: -3.0, 62: -2.0})
        eq = TagEQMixer(store).mix(_tags(bass_heavy=0.5))
        c: EQContribution = eq.contributions[0]
        assert c.tag_name == "bass_heavy"
        assert c.tag_intensity == 0.5
        # scaled_gains is in canonical order — index 0 is 31 Hz.
        assert c.scaled_gains[0] == pytest.approx(-1.5)
        assert c.scaled_gains[1] == pytest.approx(-1.0)

    def test_missing_preset_not_in_contributions(self):
        store = _store(bass_heavy={31: -3.0})
        tags = _tags(bass_heavy=0.5, warm=0.9)  # 'warm' has no preset
        eq = TagEQMixer(store).mix(tags)
        assert len(eq.contributions) == 1
        assert eq.contributions[0].tag_name == "bass_heavy"


# =============================================================================
#  Empty inputs
# =============================================================================

class TestEmpty:
    def test_empty_tagset_returns_flat(self):
        store = _store(bass_heavy={31: -3.0})
        eq = TagEQMixer(store).mix(TagSet(tags=()))
        assert eq.is_flat()
        assert eq.preamp_db == 0.0
        assert eq.contributions == ()

    def test_all_tags_missing_presets_returns_flat(self):
        store = _store()  # empty store
        eq = TagEQMixer(store).mix(_tags(x=0.5, y=0.9))
        assert eq.is_flat()
