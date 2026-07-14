"""Tag / TagSet — no external deps."""
from __future__ import annotations

import json

import pytest

from tagging.tags import Tag, TagCategory, TagSet


def _make_set() -> TagSet:
    """Deterministic set covering several categories and intensities."""
    return TagSet(tags=(
        Tag(name="bass_heavy",    category=TagCategory.BASS,    intensity=0.82),
        Tag(name="warm",          category=TagCategory.TONE,    intensity=0.64),
        Tag(name="bright",        category=TagCategory.TREBLE,  intensity=0.31),
        Tag(name="wide",          category=TagCategory.SPACE,   intensity=0.91),
        Tag(name="mid_forward",   category=TagCategory.MID,     intensity=0.10),
    ))


class TestAsDict:
    def test_matches_user_example_shape(self):
        ts = _make_set()
        d = ts.as_dict()
        assert set(d.keys()) == {"bass_heavy", "warm", "bright", "wide", "mid_forward"}
        assert d["bass_heavy"] == pytest.approx(0.82)

    def test_sorted_by_intensity_desc(self):
        ts = _make_set()
        keys = list(ts.as_dict().keys())
        # wide (0.91), bass_heavy (0.82), warm (0.64), bright (0.31), mid_forward (0.10)
        assert keys == ["wide", "bass_heavy", "warm", "bright", "mid_forward"]

    def test_sort_can_be_disabled(self):
        ts = _make_set()
        keys = list(ts.as_dict(sort_by_intensity=False).keys())
        # Original tuple order preserved
        assert keys == ["bass_heavy", "warm", "bright", "wide", "mid_forward"]


class TestGrouped:
    def test_categories_are_kept_separate(self):
        ts = _make_set()
        g = ts.grouped()
        assert set(g.keys()) == {"bass", "tone", "treble", "space", "mid"}
        assert g["bass"] == {"bass_heavy": pytest.approx(0.82)}


class TestAccessors:
    def test_by_category(self):
        ts = _make_set()
        bass = ts.by_category(TagCategory.BASS)
        assert len(bass) == 1
        assert bass[0].name == "bass_heavy"

    def test_top_returns_n_highest(self):
        ts = _make_set()
        top3 = ts.top(3)
        assert [t.name for t in top3] == ["wide", "bass_heavy", "warm"]

    def test_get_by_name(self):
        ts = _make_set()
        assert ts.get("bright").intensity == pytest.approx(0.31)
        assert ts.get("nonexistent") is None

    def test_getitem_returns_zero_for_missing(self):
        ts = _make_set()
        assert ts["nonexistent"] == 0.0
        assert ts["bass_heavy"] == pytest.approx(0.82)


class TestJsonExport:
    def test_flat_json_round_trip(self, tmp_path):
        ts = _make_set()
        p = ts.to_json(tmp_path / "tags.json")
        data = json.loads(p.read_text(encoding="utf-8"))
        assert data["bass_heavy"] == pytest.approx(0.82)
        assert data["wide"] == pytest.approx(0.91)

    def test_grouped_json(self, tmp_path):
        ts = _make_set()
        p = ts.to_json(tmp_path / "tags.json", grouped=True)
        data = json.loads(p.read_text(encoding="utf-8"))
        assert "bass" in data
        assert data["bass"]["bass_heavy"] == pytest.approx(0.82)


class TestTagCategory:
    def test_values_are_lowercase_strings(self):
        # YAML / JSON compatibility relies on this.
        for c in TagCategory:
            assert c.value == c.value.lower()
