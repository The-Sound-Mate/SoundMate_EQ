"""Intensity-mapping primitives — pure math, no external deps."""
from __future__ import annotations

import math

import pytest

from tagging.helpers import (
    bell,
    clip01,
    combine_geometric,
    combine_max,
    combine_mean,
    combine_min,
    gaussian_bell,
    ramp_down,
    ramp_up,
    sigmoid,
)


class TestClip01:
    def test_below_zero_clamps_to_zero(self):
        assert clip01(-0.5) == 0.0

    def test_above_one_clamps_to_one(self):
        assert clip01(1.7) == 1.0

    def test_inside_passthrough(self):
        assert clip01(0.42) == 0.42


class TestRampUp:
    def test_zero_below_low(self):
        assert ramp_up(0.0, low=1.0, high=2.0) == 0.0

    def test_one_above_high(self):
        assert ramp_up(3.0, low=1.0, high=2.0) == 1.0

    def test_midpoint_is_half(self):
        assert ramp_up(1.5, low=1.0, high=2.0) == pytest.approx(0.5)

    def test_degenerate_config_is_a_step(self):
        # low >= high → behaves as a hard step at `low`.
        assert ramp_up(0.9, low=1.0, high=1.0) == 0.0
        assert ramp_up(1.0, low=1.0, high=1.0) == 1.0


class TestRampDown:
    def test_one_below_low(self):
        assert ramp_down(0.0, low=1.0, high=2.0) == 1.0

    def test_zero_above_high(self):
        assert ramp_down(3.0, low=1.0, high=2.0) == 0.0

    def test_symmetric_to_ramp_up(self):
        assert ramp_down(1.5, low=1.0, high=2.0) == pytest.approx(1.0 - ramp_up(1.5, 1.0, 2.0))


class TestBell:
    def test_center_gives_one(self):
        assert bell(0.5, center=0.5, half_width=0.1) == pytest.approx(1.0)

    def test_edge_gives_zero(self):
        assert bell(0.6, center=0.5, half_width=0.1) == pytest.approx(0.0)
        assert bell(0.4, center=0.5, half_width=0.1) == pytest.approx(0.0)

    def test_outside_gives_zero(self):
        assert bell(0.9, center=0.5, half_width=0.1) == 0.0

    def test_halfway_gives_half(self):
        assert bell(0.55, center=0.5, half_width=0.1) == pytest.approx(0.5)


class TestGaussianBell:
    def test_center_gives_one(self):
        assert gaussian_bell(0.0, center=0.0, sigma=1.0) == pytest.approx(1.0)

    def test_at_1_sigma(self):
        # exp(-0.5) ≈ 0.6065
        assert gaussian_bell(1.0, center=0.0, sigma=1.0) == pytest.approx(math.exp(-0.5))


class TestSigmoid:
    def test_midpoint_gives_half(self):
        assert sigmoid(0.0, midpoint=0.0) == pytest.approx(0.5)

    def test_saturates_high(self):
        assert sigmoid(1000.0, midpoint=0.0, steepness=1.0) == pytest.approx(1.0)

    def test_saturates_low(self):
        assert sigmoid(-1000.0, midpoint=0.0, steepness=1.0) == pytest.approx(0.0)


class TestCombinators:
    def test_geometric_all_high(self):
        assert combine_geometric(1.0, 1.0, 1.0) == pytest.approx(1.0)

    def test_geometric_one_zero_kills_all(self):
        # Geometric mean of anything containing 0 is 0.
        assert combine_geometric(0.9, 0.9, 0.0) == 0.0

    def test_geometric_empty(self):
        assert combine_geometric() == 0.0

    def test_min_returns_weakest(self):
        assert combine_min(0.7, 0.3, 0.9) == 0.3

    def test_max_returns_strongest(self):
        assert combine_max(0.7, 0.3, 0.9) == 0.9

    def test_max_clips_above_one(self):
        assert combine_max(1.5, 0.1) == 1.0

    def test_mean_arithmetic(self):
        assert combine_mean(0.2, 0.4, 0.6) == pytest.approx(0.4)
