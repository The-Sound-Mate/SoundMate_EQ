"""FinalEQ dataclass — round-trip, JSON, field consistency."""
from __future__ import annotations

import json

import pytest

from eq.mixer import FinalEQ
from eq.presets import BAND_FREQUENCIES_HZ


class TestFieldSchema:
    def test_ten_hz_fields_present(self):
        # Every band frequency must have a matching hz_N attribute.
        for hz in BAND_FREQUENCIES_HZ:
            assert hasattr(FinalEQ(), f"hz_{hz}")

    def test_defaults_to_flat(self):
        eq = FinalEQ()
        assert eq.is_flat()
        assert eq.preamp_db == 0.0
        assert eq.contributions == ()


class TestConstruction:
    def test_from_gains_dict(self):
        eq = FinalEQ.from_gains({31: -3.0, 62: -2.0}, preamp_db=-1.0)
        assert eq.hz_31 == -3.0
        assert eq.hz_62 == -2.0
        assert eq.hz_125 == 0.0
        assert eq.preamp_db == -1.0

    def test_from_gains_tuple(self):
        gains = (-3.0, -2.0, -1.0, 0, 0, 0, 0, -2.0, -2.0, 0)
        eq = FinalEQ.from_gains(gains, preamp_db=-0.5)
        assert eq.hz_31 == -3.0
        assert eq.hz_16000 == 0.0
        assert eq.preamp_db == -0.5

    def test_from_gains_tuple_wrong_length_raises(self):
        with pytest.raises(ValueError):
            FinalEQ.from_gains((1.0, 2.0, 3.0))

    def test_flat_factory(self):
        eq = FinalEQ.flat()
        assert eq.is_flat()


class TestAccessors:
    def test_gains_canonical_order(self):
        eq = FinalEQ.from_gains({31: -3.0, 16000: -1.0})
        gains = eq.gains()
        assert len(gains) == 10
        assert gains[0] == -3.0
        assert gains[-1] == -1.0

    def test_as_dict(self):
        eq = FinalEQ.from_gains({31: -3.0, 62: -2.0})
        d = eq.as_dict()
        assert d == {
            31: -3.0, 62: -2.0, 125: 0.0, 250: 0.0, 500: 0.0,
            1000: 0.0, 2000: 0.0, 4000: 0.0, 8000: 0.0, 16000: 0.0,
        }

    def test_gain_at(self):
        eq = FinalEQ.from_gains({125: -1.5})
        assert eq.gain_at(125) == -1.5
        assert eq.gain_at(31) == 0.0

    def test_gain_at_invalid_raises(self):
        eq = FinalEQ()
        with pytest.raises(ValueError):
            eq.gain_at(60)  # not a canonical band

    def test_is_flat_detects_non_flat(self):
        assert FinalEQ.from_gains({62: -0.1}).is_flat() is False


class TestJSON:
    def test_to_dict_shape(self):
        eq = FinalEQ.from_gains({31: -3.0, 62: -2.0}, preamp_db=-1.5)
        d = eq.to_dict()
        assert "bands" in d and "preamp_db" in d
        assert d["preamp_db"] == -1.5
        assert d["bands"]["31"] == -3.0
        assert d["bands"]["62"] == -2.0
        # All 10 bands are represented, even zeros.
        assert len(d["bands"]) == 10

    def test_to_json_and_from_json_round_trip(self, tmp_path):
        eq = FinalEQ.from_gains({31: -3.0, 62: -2.0, 4000: -2.5}, preamp_db=-1.5)
        out = eq.to_json(tmp_path / "eq.json")

        loaded = FinalEQ.from_json(out)
        assert loaded.hz_31 == eq.hz_31
        assert loaded.hz_62 == eq.hz_62
        assert loaded.hz_4000 == eq.hz_4000
        assert loaded.preamp_db == eq.preamp_db

    def test_json_is_strict(self, tmp_path):
        # Output must be strict JSON that a non-Python consumer can parse.
        eq = FinalEQ.from_gains({31: -3.0}, preamp_db=-0.5)
        out = eq.to_json(tmp_path / "eq.json")
        text = out.read_text(encoding="utf-8")
        json.loads(text)  # raises if non-strict


class TestFrozen:
    def test_dataclass_is_immutable(self):
        eq = FinalEQ.from_gains({31: -3.0})
        with pytest.raises(Exception):
            eq.hz_31 = 0.0  # type: ignore[misc]
