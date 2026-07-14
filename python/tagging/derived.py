"""Derived features — one-shot derivations reused across many tag rules.

Rules should read from :class:`DerivedFeatures` rather than re-computing
band-energy ratios or unwrapping :class:`FeatureVector` fields.  This keeps
each rule short and makes changing a band definition (say, extending the
"vocal" range) a single-line edit.

Frequency bands used throughout:

    sub_bass    :    20 –    60 Hz
    bass        :    60 –   250 Hz
    low_mid     :   250 –   500 Hz
    mid         :   500 –  2000 Hz
    high_mid    :  2000 –  4000 Hz
    presence    :  4000 –  6000 Hz
    brilliance  :  6000 – 12000 Hz
    air         : 12000 – 20000 Hz

Special-purpose ranges (for vocal / instrument / drum-piece rules) sit on top
of the primary bands and may overlap them.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray

from features.extractor import FeatureVector


# =============================================================================
#  DerivedFeatures dataclass
# =============================================================================

@dataclass(frozen=True, slots=True)
class DerivedFeatures:
    """Cache of per-track derivations used by rule functions.

    All ``*_ratio`` fields are normalized fractions of total spectral energy
    (they don't necessarily sum to 1 because some ranges overlap or leave
    gaps).  Scalar fields are passed through from :class:`FeatureVector`.
    """

    # ---- Primary bands (do sum to ≈ 1) --------------------------------------
    sub_bass_ratio: float          # 20-60 Hz
    bass_ratio: float              # 60-250 Hz
    low_mid_ratio: float           # 250-500 Hz
    mid_ratio: float               # 500-2000 Hz
    high_mid_ratio: float          # 2000-4000 Hz
    presence_ratio: float          # 4000-6000 Hz
    brilliance_ratio: float        # 6000-12000 Hz
    air_ratio: float               # 12000-20000 Hz

    # ---- Overlapping special ranges (vocal / character) --------------------
    vocal_band_ratio: float        # 300-3400 Hz  (speech intelligibility)
    sibilance_ratio: float         # 5000-9000 Hz
    nasal_band_ratio: float        # 800-1500 Hz
    boxy_band_ratio: float         # 250-500 Hz   (same as low_mid, alias)
    honky_band_ratio: float        # 500-1000 Hz

    # ---- Instrument profile ranges (best-effort heuristics) ---------------
    kick_thump_ratio: float        # 40-100 Hz
    kick_click_ratio: float        # 2000-5000 Hz
    snare_body_ratio: float        # 150-300 Hz
    snare_snap_ratio: float        # 4000-6000 Hz
    cymbal_range_ratio: float      # 5000-16000 Hz
    piano_range_ratio: float       # 250-4000 Hz
    guitar_range_ratio: float      # 100-2000 Hz
    strings_range_ratio: float     # 200-3000 Hz
    synth_hf_ratio: float          # 3000-12000 Hz

    # ---- Spectrum scalars --------------------------------------------------
    centroid_hz: float
    bandwidth_hz: float
    rolloff_hz: float
    flatness: float
    contrast_mean: float           # mean over spectral_contrast_mean bands

    # ---- Loudness ----------------------------------------------------------
    peak_dbfs: float
    rms_db: float
    lufs_integrated: float
    dynamic_range_db: float
    crest_factor_db: float         # peak_dbfs − rms_db (same as dynamic_range_db)

    # ---- Stereo ------------------------------------------------------------
    stereo_width: float
    lr_balance: float
    lr_correlation: float
    is_stereo: bool

    # ---- Rhythm / HPSS / Tempo / Pitch ------------------------------------
    onset_rate_per_sec: float
    harmonic_ratio: float
    percussive_ratio: float
    bpm: float
    voiced_ratio: float
    f0_median_hz: float

    # ---- RMS envelope statistics ------------------------------------------
    rms_env_std_db: float          # variability of loudness over time

    # =========================================================================
    #  Factory
    # =========================================================================
    @classmethod
    def from_feature_vector(cls, fv: FeatureVector) -> "DerivedFeatures":
        """Compute every derivation once for the given :class:`FeatureVector`."""
        # LTAS = long-term average magnitude spectrum, shape (n_bins,).
        ltas: NDArray[np.float32] = fv.spectrum.fft_magnitude
        # We use magnitude² as an energy proxy.
        energy = np.square(ltas).astype(np.float64)
        total_energy = float(energy.sum()) + 1e-12

        # Bin → frequency mapping is: freq_k = k * sr / n_fft.
        n_bins = energy.shape[0]
        n_fft = fv.spectrum.n_fft
        freqs = np.arange(n_bins, dtype=np.float64) * fv.samplerate / n_fft

        def _ratio(low_hz: float, high_hz: float) -> float:
            """Fraction of total energy in ``[low_hz, high_hz)``."""
            mask = (freqs >= low_hz) & (freqs < high_hz)
            return float(energy[mask].sum() / total_energy)

        # ------------------------------------------------------------- summary
        # Loudness convenience: crest factor is peak minus RMS in dB.
        crest = float(fv.loudness.peak_dbfs - fv.loudness.rms_db)

        # RMS envelope variability — proxy for macro-dynamics.
        rms_env = fv.loudness.rms_envelope
        rms_env_std = float(np.std(rms_env)) if rms_env.size else 0.0

        # Spectral contrast mean over bands (already time-averaged).
        contrast_scalar = float(fv.spectrum.spectral_contrast_mean.mean()) \
            if fv.spectrum.spectral_contrast_mean.size else 0.0

        return cls(
            # Primary bands
            sub_bass_ratio=_ratio(20, 60),
            bass_ratio=_ratio(60, 250),
            low_mid_ratio=_ratio(250, 500),
            mid_ratio=_ratio(500, 2000),
            high_mid_ratio=_ratio(2000, 4000),
            presence_ratio=_ratio(4000, 6000),
            brilliance_ratio=_ratio(6000, 12000),
            air_ratio=_ratio(12000, 20000),
            # Special ranges
            vocal_band_ratio=_ratio(300, 3400),
            sibilance_ratio=_ratio(5000, 9000),
            nasal_band_ratio=_ratio(800, 1500),
            boxy_band_ratio=_ratio(250, 500),
            honky_band_ratio=_ratio(500, 1000),
            # Instrument profiles
            kick_thump_ratio=_ratio(40, 100),
            kick_click_ratio=_ratio(2000, 5000),
            snare_body_ratio=_ratio(150, 300),
            snare_snap_ratio=_ratio(4000, 6000),
            cymbal_range_ratio=_ratio(5000, 16000),
            piano_range_ratio=_ratio(250, 4000),
            guitar_range_ratio=_ratio(100, 2000),
            strings_range_ratio=_ratio(200, 3000),
            synth_hf_ratio=_ratio(3000, 12000),
            # Spectrum scalars
            centroid_hz=fv.spectrum.spectral_centroid_mean,
            bandwidth_hz=fv.spectrum.spectral_bandwidth_mean,
            rolloff_hz=fv.spectrum.spectral_rolloff_mean,
            flatness=fv.spectrum.spectral_flatness_mean,
            contrast_mean=contrast_scalar,
            # Loudness
            peak_dbfs=fv.loudness.peak_dbfs,
            rms_db=fv.loudness.rms_db,
            lufs_integrated=fv.loudness.lufs_integrated,
            dynamic_range_db=fv.loudness.dynamic_range_db,
            crest_factor_db=crest,
            # Stereo
            stereo_width=fv.stereo.stereo_width,
            lr_balance=fv.stereo.lr_balance,
            lr_correlation=fv.stereo.lr_correlation,
            is_stereo=fv.stereo.is_stereo,
            # Rhythm / HPSS / Tempo / Pitch
            onset_rate_per_sec=fv.rhythm.onset_rate_per_sec,
            harmonic_ratio=fv.hpss.harmonic_ratio,
            percussive_ratio=fv.hpss.percussive_ratio,
            bpm=fv.tempo.bpm,
            voiced_ratio=fv.pitch.voiced_ratio,
            f0_median_hz=fv.pitch.f0_median_hz,
            # Loudness envelope
            rms_env_std_db=rms_env_std,
        )
