"""SoundMate EQ — CLI entry point.

Runs the full 8-step pipeline for a single audio file:

    1. MP3/WAV input   ─┐
    2. Audio Loader     │
    3. Feature Extractor│
    4. Tag Generator    │  ← one orchestration in ``run_pipeline()``
    5. EQ Preset Loader │
    6. EQ Engine        │
    7. Final EQ 생성    │
    8. JSON 저장        ─┘

Usage::

    python main.py music.mp3
    python main.py music.wav -v                    # verbose logs
    python main.py music.mp3 --sr 44100            # analysis sample rate
    python main.py music.mp3 --max-gain 6          # ±6 dB clip instead of ±12
    python main.py music.mp3 --auto-reload         # watch YAML presets

Every stage is wrapped in ``try/except`` so failures produce a clear log line
and a distinct exit code — no bare tracebacks unless ``-v -v`` is passed.
Output artefacts are written to ``output/`` as ``feature.json``, ``tag.json``,
``eq.json`` in that order.
"""
from __future__ import annotations

import argparse
import logging
import sys
from dataclasses import dataclass
from pathlib import Path

# Force UTF-8 on stdout/stderr so Rich's Unicode box characters and em-dashes
# work even under the legacy Windows console (cp949 / cp1252).  Must run
# before Rich imports anything that captures the file streams.
for _stream in (sys.stdout, sys.stderr):
    reconfigure = getattr(_stream, "reconfigure", None)
    if reconfigure is not None:
        try:
            reconfigure(encoding="utf-8", errors="replace")
        except (OSError, ValueError):
            pass

from rich import box
from rich.console import Console
from rich.panel import Panel
from rich.table import Table

from audio import AudioData, AudioLoader, AudioLoadError
from eq import FinalEQ, MixerConfig, PresetStore, TagEQMixer
from features.extractor import FeatureExtractor, FeatureVector
from tagging import TagGenerator, TagSet

logger = logging.getLogger("soundmate_eq")

PROJECT_ROOT: Path = Path(__file__).resolve().parent
OUTPUT_DIR: Path = PROJECT_ROOT / "output"
CONFIG_DIR: Path = PROJECT_ROOT / "config"


# =============================================================================
#  CLI
# =============================================================================

@dataclass(frozen=True, slots=True)
class CLIArgs:
    """Parsed command-line arguments."""

    audio_path: Path
    output_dir: Path
    sample_rate: int | None
    max_gain_db: float
    min_gain_db: float
    min_intensity: float
    auto_reload: bool
    include_arrays: bool
    verbose: int
    quiet: bool


def parse_args(argv: list[str] | None = None) -> CLIArgs:
    """Parse ``argv`` (or ``sys.argv[1:]`` if ``None``) into :class:`CLIArgs`."""
    p = argparse.ArgumentParser(
        prog="soundmate-eq",
        description="Analyze an audio file and produce a 10-band EQ curve.",
    )
    p.add_argument("audio_path", type=Path, help="Path to an MP3 / WAV / FLAC file.")

    p.add_argument(
        "-o", "--output-dir", type=Path, default=OUTPUT_DIR,
        help=f"Directory to write JSON files into (default: {OUTPUT_DIR.name}/).",
    )
    p.add_argument(
        "--sr", dest="sample_rate", type=int, default=22050,
        help="Analysis sample rate. 0 = keep native. Default 22050.",
    )
    p.add_argument(
        "--max-gain", dest="max_gain_db", type=float, default=12.0,
        help="Per-band positive clip in dB (default +12).",
    )
    p.add_argument(
        "--min-gain", dest="min_gain_db", type=float, default=-12.0,
        help="Per-band negative clip in dB (default −12).",
    )
    p.add_argument(
        "--min-intensity", type=float, default=0.0,
        help="Skip tags below this intensity in the EQ sum (default 0.0).",
    )
    p.add_argument(
        "--auto-reload", action="store_true",
        help="Watch config/eq_presets.yaml and reload on change.",
    )
    p.add_argument(
        "--no-arrays", dest="include_arrays", action="store_false",
        help="Omit per-frame arrays from feature.json (scalars only).",
    )
    p.add_argument("-v", "--verbose", action="count", default=0,
                   help="Increase log verbosity (-v = INFO, -vv = DEBUG).")
    p.add_argument("-q", "--quiet", action="store_true",
                   help="Suppress everything except errors.")

    ns = p.parse_args(argv)
    return CLIArgs(
        audio_path=ns.audio_path,
        output_dir=ns.output_dir,
        sample_rate=None if ns.sample_rate == 0 else ns.sample_rate,
        max_gain_db=ns.max_gain_db,
        min_gain_db=ns.min_gain_db,
        min_intensity=ns.min_intensity,
        auto_reload=ns.auto_reload,
        include_arrays=ns.include_arrays,
        verbose=ns.verbose,
        quiet=ns.quiet,
    )


def _configure_logging(verbose: int, quiet: bool) -> None:
    """Set up root logging.  ``-q`` overrides ``-v``."""
    if quiet:
        level = logging.ERROR
    elif verbose >= 2:
        level = logging.DEBUG
    elif verbose >= 1:
        level = logging.INFO
    else:
        level = logging.WARNING

    logging.basicConfig(
        level=level,
        format="[%(asctime)s] %(levelname)-5s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )


# =============================================================================
#  Pipeline
# =============================================================================

@dataclass(frozen=True, slots=True)
class PipelineOutput:
    """Everything produced by one pipeline run — for display + tests."""

    audio: AudioData
    features: FeatureVector
    tags: TagSet
    final_eq: FinalEQ
    feature_path: Path
    tag_path: Path
    eq_path: Path


def run_pipeline(args: CLIArgs) -> PipelineOutput:
    """Execute steps 2–8 in order.

    Each step is logged; failures propagate as-is so ``main()`` can map them
    to distinct exit codes.
    """
    args.output_dir.mkdir(parents=True, exist_ok=True)

    # ---- Step 2: Audio Loader ----------------------------------------------
    logger.info("[2/8] loading audio")
    loader = AudioLoader(target_sr=args.sample_rate)
    audio: AudioData = loader.load(args.audio_path)
    logger.debug("      audio: sr=%d, ch=%d, dur=%.2fs",
                 audio.samplerate, audio.channels, audio.duration)

    # ---- Step 3: Feature Extractor -----------------------------------------
    logger.info("[3/8] extracting features")
    extractor = FeatureExtractor()
    features: FeatureVector = extractor.extract(audio)

    # ---- Step 4: Tag Generator ---------------------------------------------
    logger.info("[4/8] generating tags")
    tag_gen = TagGenerator()
    tags: TagSet = tag_gen.generate(features)
    logger.debug("      %d tags produced", len(tags))

    # ---- Step 5: EQ Preset Loader ------------------------------------------
    logger.info("[5/8] loading EQ presets")
    presets = PresetStore.load_default(auto_reload=args.auto_reload)
    logger.debug("      %d presets loaded", len(presets))

    # ---- Step 6-7: EQ Engine → Final EQ ------------------------------------
    logger.info("[6/8] computing final EQ")
    mixer = TagEQMixer(
        store=presets,
        config=MixerConfig(
            min_intensity=args.min_intensity,
            max_gain_db=args.max_gain_db,
            min_gain_db=args.min_gain_db,
        ),
    )
    final_eq: FinalEQ = mixer.mix(tags)
    logger.info("[7/8] final EQ ready (preamp=%+.2f dB)", final_eq.preamp_db)

    # ---- Step 8: JSON 저장 --------------------------------------------------
    logger.info("[8/8] writing JSON to %s", args.output_dir)
    feature_path = args.output_dir / "feature.json"
    tag_path = args.output_dir / "tag.json"
    eq_path = args.output_dir / "eq.json"

    features.to_json(feature_path, include_arrays=args.include_arrays)
    tags.to_json(tag_path)
    final_eq.to_json(eq_path)

    return PipelineOutput(
        audio=audio,
        features=features,
        tags=tags,
        final_eq=final_eq,
        feature_path=feature_path,
        tag_path=tag_path,
        eq_path=eq_path,
    )


# =============================================================================
#  Rich console display
# =============================================================================

def display(out: PipelineOutput, console: Console) -> None:
    """Pretty-print the pipeline result to the terminal."""
    console.rule(f"[bold]SoundMate EQ[/bold] — {out.audio.source_path}")
    _render_features(out.features, console)
    _render_tags(out.tags, console)
    _render_final_eq(out.final_eq, console)
    _render_files(out, console)


def _render_features(fv: FeatureVector, console: Console) -> None:
    """Two-column table of the headline feature scalars."""
    table = Table(title="Features", box=box.SIMPLE_HEAVY, show_header=False, expand=False)
    table.add_column(style="cyan", no_wrap=True)
    table.add_column()

    def row(k: str, v: str) -> None:
        table.add_row(k, v)

    row("samplerate",     f"{fv.samplerate} Hz")
    row("duration",       f"{fv.duration_sec:.2f} s")
    row("channels",       str(fv.channels))
    row("tempo",          f"{fv.tempo.bpm:.2f} BPM  [dim]({fv.tempo.backend})[/dim]")
    row("f0 median",      f"{fv.pitch.f0_median_hz:.1f} Hz  "
                          f"[dim](voiced {fv.pitch.voiced_ratio:.2f})[/dim]")
    row("centroid",       f"{fv.spectrum.spectral_centroid_mean:.0f} Hz")
    row("rolloff",        f"{fv.spectrum.spectral_rolloff_mean:.0f} Hz")
    row("flatness",       f"{fv.spectrum.spectral_flatness_mean:.3f}")
    row("LUFS",           f"{fv.loudness.lufs_integrated:.1f}")
    row("peak",           f"{fv.loudness.peak_dbfs:+.1f} dBFS")
    row("dynamic range",  f"{fv.loudness.dynamic_range_db:.1f} dB")
    row("stereo",         f"width={fv.stereo.stereo_width:.2f}, "
                          f"balance={fv.stereo.lr_balance:+.2f}, "
                          f"stereo={fv.stereo.is_stereo}")
    row("onset rate",     f"{fv.rhythm.onset_rate_per_sec:.2f} /s")
    row("HPSS",           f"H={fv.hpss.harmonic_ratio:.2f} / "
                          f"P={fv.hpss.percussive_ratio:.2f}")

    console.print(table)


def _render_tags(tags: TagSet, console: Console) -> None:
    """Top-N tags with a 20-char intensity bar."""
    top = tags.top(10)
    if not top:
        console.print("[dim](no tags fired)[/dim]")
        return

    table = Table(title="Tags (top 10)", box=box.SIMPLE_HEAVY, expand=False)
    table.add_column("tag", style="cyan", no_wrap=True)
    table.add_column("category", style="dim")
    table.add_column("intensity", justify="right")
    table.add_column("", no_wrap=True)

    for t in top:
        filled = int(round(t.intensity * 20))
        bar = "█" * filled + "░" * (20 - filled)
        table.add_row(t.name, t.category.value, f"{t.intensity:.2f}",
                      f"[green]{bar}[/green]")

    console.print(table)


def _render_final_eq(eq: FinalEQ, console: Console) -> None:
    """10-band curve with a centred ASCII bar per band."""
    title = f"Final EQ  (preamp: [bold]{eq.preamp_db:+.2f} dB[/bold])"
    table = Table(title=title, box=box.SIMPLE_HEAVY, expand=False)
    table.add_column("freq", style="cyan", justify="right", no_wrap=True)
    table.add_column("gain", justify="right", no_wrap=True)
    table.add_column("", no_wrap=True)

    # Bar centred on 0 dB; ±12 dB spans 24 characters, so 12 = zero position.
    WIDTH = 24
    ZERO = WIDTH // 2

    for hz, gain in eq.as_dict().items():
        # Clamp visualization to ±12 dB so bars stay inside the fixed width.
        clipped = max(-12.0, min(12.0, gain))
        slot = int(round((clipped + 12.0) / 24.0 * WIDTH))
        slot = max(0, min(WIDTH, slot))

        bar_chars: list[str] = [" "] * (WIDTH + 1)
        bar_chars[ZERO] = "│"
        if slot > ZERO:
            for i in range(ZERO + 1, slot + 1):
                bar_chars[i] = "█"
        elif slot < ZERO:
            for i in range(slot, ZERO):
                bar_chars[i] = "█"

        if gain > 0.1:
            color = "green"
        elif gain < -0.1:
            color = "red"
        else:
            color = "white"

        table.add_row(
            f"{hz} Hz",
            f"{gain:+.2f} dB",
            f"[{color}]{''.join(bar_chars)}[/{color}]",
        )
    console.print(table)


def _render_files(out: PipelineOutput, console: Console) -> None:
    """Report where the three JSON files landed."""
    body = "\n".join([
        f"[cyan]feature[/cyan] → {out.feature_path}",
        f"[cyan]tag    [/cyan] → {out.tag_path}",
        f"[cyan]eq     [/cyan] → {out.eq_path}",
    ])
    console.print(Panel(body, title="Saved", expand=False, box=box.SIMPLE))


# =============================================================================
#  Entry point — clean exit-code contract
# =============================================================================

# Exit codes
EXIT_OK = 0
EXIT_FILE_NOT_FOUND = 1
EXIT_DECODE_FAILED = 2
EXIT_PIPELINE_ERROR = 3
EXIT_ARG_ERROR = 4


def main(argv: list[str] | None = None) -> int:
    """CLI entry point.  Returns a process exit code (see ``EXIT_*``)."""
    try:
        args = parse_args(argv)
    except SystemExit as e:  # argparse calls sys.exit on error; propagate cleanly.
        return int(e.code) if isinstance(e.code, int) else EXIT_ARG_ERROR

    _configure_logging(args.verbose, args.quiet)

    console = Console()

    try:
        out = run_pipeline(args)
    except FileNotFoundError as e:
        logger.error("file not found: %s", e)
        return EXIT_FILE_NOT_FOUND
    except AudioLoadError as e:
        logger.error("decode failed: %s", e)
        return EXIT_DECODE_FAILED
    except Exception as e:
        # For any other failure show a short line at ERROR, full trace at DEBUG.
        logger.error("pipeline failed: %s: %s", type(e).__name__, e)
        logger.debug("traceback", exc_info=True)
        return EXIT_PIPELINE_ERROR

    if not args.quiet:
        display(out, console)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
