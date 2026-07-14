"""EQ preset dataclasses + YAML loading + auto-reload behaviour."""
from __future__ import annotations

import time

import pytest

from eq.presets import (
    BAND_FREQUENCIES_HZ,
    EQBand,
    EQPreset,
    PresetStore,
    is_valid_band,
)


# =============================================================================
#  EQPreset
# =============================================================================

class TestFromGains:
    def test_missing_bands_default_to_zero(self):
        p = EQPreset.from_gains("bass_heavy", {31: -3.0, 62: -2.0})
        assert p.gain_at(31) == -3.0
        assert p.gain_at(62) == -2.0
        assert p.gain_at(125) == 0.0
        assert p.gain_at(16000) == 0.0

    def test_all_ten_bands_always_present(self):
        p = EQPreset.from_gains("x", {})
        assert len(p.bands) == len(BAND_FREQUENCIES_HZ)
        assert tuple(b.frequency_hz for b in p.bands) == BAND_FREQUENCIES_HZ

    def test_invalid_frequency_raises(self):
        with pytest.raises(ValueError):
            EQPreset.from_gains("x", {60: -3.0})  # 60 is not one of the 10 bands


class TestQueries:
    def test_gains_in_canonical_order(self):
        p = EQPreset.from_gains("x", {31: -1, 16000: -2})
        gains = p.gains()
        assert gains[0] == -1.0
        assert gains[-1] == -2.0
        assert all(g == 0.0 for g in gains[1:-1])

    def test_as_dict(self):
        p = EQPreset.from_gains("x", {125: -1.5})
        d = p.as_dict()
        assert d[125] == -1.5
        assert d[31] == 0.0

    def test_gain_at_unknown_freq_raises(self):
        p = EQPreset.from_gains("x", {})
        with pytest.raises(ValueError):
            p.gain_at(60)

    def test_flat_helper(self):
        p = EQPreset.flat("x")
        assert p.is_flat()
        assert all(b.gain_db == 0.0 for b in p.bands)


class TestIsValidBand:
    def test_accepts_canonical_freqs(self):
        for hz in BAND_FREQUENCIES_HZ:
            assert is_valid_band(hz)

    def test_rejects_nearby_freqs(self):
        assert not is_valid_band(60)
        assert not is_valid_band(1200)


# =============================================================================
#  PresetStore — YAML loading
# =============================================================================

def _write_yaml(path, content: str) -> None:
    path.write_text(content, encoding="utf-8")


class TestYAMLLoading:
    def test_matches_user_example_format(self, tmp_path):
        yaml_path = tmp_path / "presets.yaml"
        _write_yaml(yaml_path, """
bass_heavy:
    31: -3
    62: -2
    125: -1

bright:
    4000: -2
    8000: -2

vocal_forward:
    1000: -1
    2000: -2
    4000: -1
""")
        store = PresetStore.load(yaml_path)

        assert store.get("bass_heavy").gain_at(31) == -3.0
        assert store.get("bright").gain_at(4000) == -2.0
        assert store.get("vocal_forward").gain_at(2000) == -2.0

    def test_missing_tag_returns_none(self, tmp_path):
        yaml_path = tmp_path / "presets.yaml"
        _write_yaml(yaml_path, "bass_heavy:\n  31: -3\n")
        store = PresetStore.load(yaml_path)
        assert store.get("does_not_exist") is None

    def test_empty_file_is_valid(self, tmp_path):
        yaml_path = tmp_path / "presets.yaml"
        _write_yaml(yaml_path, "")
        store = PresetStore.load(yaml_path)
        assert len(store) == 0

    def test_metadata_keys_ignored(self, tmp_path):
        # Non-numeric keys inside a preset block are documentation, not bands.
        yaml_path = tmp_path / "presets.yaml"
        _write_yaml(yaml_path, """
bass_heavy:
    description: cuts bass
    31: -3
""")
        store = PresetStore.load(yaml_path)
        assert store.get("bass_heavy").gain_at(31) == -3.0
        assert store.get("bass_heavy").gain_at(62) == 0.0

    def test_wrapped_bands_key(self, tmp_path):
        yaml_path = tmp_path / "presets.yaml"
        _write_yaml(yaml_path, """
bass_heavy:
    bands:
        31: -3
        62: -2
""")
        store = PresetStore.load(yaml_path)
        assert store.get("bass_heavy").gain_at(31) == -3.0
        assert store.get("bass_heavy").gain_at(62) == -2.0

    def test_invalid_band_frequency_raises(self, tmp_path):
        # 60 is not one of the 10 canonical bands — must fail loudly, not silently.
        yaml_path = tmp_path / "presets.yaml"
        _write_yaml(yaml_path, "bass_heavy:\n  60: -3\n")
        with pytest.raises(ValueError):
            PresetStore.load(yaml_path)


# =============================================================================
#  Auto-reload
# =============================================================================

class TestAutoReload:
    def test_reload_picks_up_changes(self, tmp_path):
        yaml_path = tmp_path / "presets.yaml"
        _write_yaml(yaml_path, "bass_heavy:\n  31: -3\n")
        store = PresetStore.load(yaml_path, auto_reload=True)
        assert store.get("bass_heavy").gain_at(31) == -3.0

        # Rewrite YAML with different values.  Bump mtime forward so the
        # store detects the change even on file systems with coarse mtime.
        _write_yaml(yaml_path, "bass_heavy:\n  31: -6\n")
        future = time.time() + 2
        import os
        os.utime(yaml_path, (future, future))

        assert store.get("bass_heavy").gain_at(31) == -6.0

    def test_no_reload_when_disabled(self, tmp_path):
        yaml_path = tmp_path / "presets.yaml"
        _write_yaml(yaml_path, "bass_heavy:\n  31: -3\n")
        store = PresetStore.load(yaml_path, auto_reload=False)
        assert store.get("bass_heavy").gain_at(31) == -3.0

        _write_yaml(yaml_path, "bass_heavy:\n  31: -9\n")
        # Static store keeps the initial value regardless of edits.
        assert store.get("bass_heavy").gain_at(31) == -3.0

    def test_force_reload(self, tmp_path):
        yaml_path = tmp_path / "presets.yaml"
        _write_yaml(yaml_path, "bass_heavy:\n  31: -3\n")
        store = PresetStore.load(yaml_path, auto_reload=False)

        _write_yaml(yaml_path, "bass_heavy:\n  31: -9\n")
        store.reload()
        assert store.get("bass_heavy").gain_at(31) == -9.0

    def test_reload_survives_missing_file(self, tmp_path):
        """If the file disappears mid-run we keep the last-known-good copy."""
        yaml_path = tmp_path / "presets.yaml"
        _write_yaml(yaml_path, "bass_heavy:\n  31: -3\n")
        store = PresetStore.load(yaml_path, auto_reload=True)

        yaml_path.unlink()
        # get() must not raise — falls back to the cached preset.
        assert store.get("bass_heavy").gain_at(31) == -3.0


# =============================================================================
#  Round-trip preservation
# =============================================================================

class TestConsistency:
    def test_from_gains_round_trips_via_as_dict(self):
        original = {31: -3.0, 62: -1.5, 4000: -2.0}
        p = EQPreset.from_gains("bass_heavy", original)
        rebuilt = EQPreset.from_gains("bass_heavy", p.as_dict())
        assert p.gains() == rebuilt.gains()

    def test_band_dataclass_immutable(self):
        b = EQBand(frequency_hz=31, gain_db=-3.0)
        with pytest.raises(Exception):  # dataclass frozen → FrozenInstanceError
            b.gain_db = -5.0  # type: ignore[misc]
