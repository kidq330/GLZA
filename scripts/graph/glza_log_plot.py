#!/usr/bin/env python3
"""
Parse GLZA compression stderr (PRINTON build) and plot iteration stats.

Typical log lines:

  Start 0.0000 score[0-2072] = 44.40134-43.27885
  88: grammar size: 321724120, 11232 rules, 8.0148 bits/sym, o0e 322319447 bytes

Run via the venv wrapper (recommended):

  ./scripts/glza_log_plot.sh glza.stderr
  head -n 5000 glza.stderr | ./scripts/glza_log_plot.sh -o glza_plots
  ./scripts/glza_log_plot.sh --watch -o glza_plots glza.stderr

Or after: python3 -m venv scripts/.venv && scripts/.venv/bin/pip install -r scripts/requirements-glza-plot.txt

  scripts/.venv/bin/python scripts/glza_log_plot.py glza.stderr
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO, Iterable, List, Optional, TextIO

RE_START = re.compile(r"Start\s+([\d.]+)")
RE_SCORE = re.compile(
    r"score\[0-(\d+)\]\s*=\s*([\d.]+)-([\d.]+)",
    re.IGNORECASE,
)
RE_GRAMMAR = re.compile(
    r"^(\d+):\s*grammar size:\s*(\d+),\s*(\d+)\s*rules,\s*"
    r"([\d.]+)\s*bits/sym,\s*o0e\s*(\d+)\s*bytes\s*$",
    re.IGNORECASE,
)
RE_PASS_FULL = re.compile(
    r"PASS\s+(\d+):\s*grammar size\s+(\d+),\s*(\d+)\s*production rules\s*$",
    re.IGNORECASE,
)
RE_PASS_CR = re.compile(
    r"PASS\s+(\d+):\s*grammar size\s+(\d+),\s*(\d+)\s*production rules",
    re.IGNORECASE,
)


@dataclass
class ScoreSnapshot:
    cycle_start: Optional[float] = None
    candidates: Optional[int] = None
    score_max: Optional[float] = None
    score_min: Optional[float] = None


@dataclass
class Iteration:
    pass_num: int
    grammar_size: int
    rules: int
    bits_per_sym: Optional[float] = None
    o0e_bytes: Optional[int] = None
    score: ScoreSnapshot = field(default_factory=ScoreSnapshot)


def _strip_crlf(line: str) -> str:
    if "\r" in line:
        line = line.split("\r")[-1]
    return line.strip()


def parse_lines(lines: Iterable[str]) -> List[Iteration]:
    pending = ScoreSnapshot()
    iterations: List[Iteration] = []
    seen_pass: set[int] = set()

    for raw in lines:
        line = _strip_crlf(raw)
        if not line or line == ".":
            continue

        m = RE_START.search(line)
        if m:
            pending.cycle_start = float(m.group(1))

        m = RE_SCORE.search(line)
        if m:
            pending.candidates = int(m.group(1))
            pending.score_max = float(m.group(2))
            pending.score_min = float(m.group(3))

        m = RE_GRAMMAR.match(line)
        if m:
            p = int(m.group(1))
            if p not in seen_pass:
                seen_pass.add(p)
                it = Iteration(
                    pass_num=p,
                    grammar_size=int(m.group(2)),
                    rules=int(m.group(3)),
                    bits_per_sym=float(m.group(4)),
                    o0e_bytes=int(m.group(5)),
                )
                it.score = ScoreSnapshot(
                    cycle_start=pending.cycle_start,
                    candidates=pending.candidates,
                    score_max=pending.score_max,
                    score_min=pending.score_min,
                )
                pending = ScoreSnapshot()
                iterations.append(it)
            continue

        m = RE_PASS_FULL.match(line) or RE_PASS_CR.search(line)
        if m:
            p = int(m.group(1))
            if p not in seen_pass:
                seen_pass.add(p)
                it = Iteration(
                    pass_num=p,
                    grammar_size=int(m.group(2)),
                    rules=int(m.group(3)),
                )
                it.score = ScoreSnapshot(
                    cycle_start=pending.cycle_start,
                    candidates=pending.candidates,
                    score_max=pending.score_max,
                    score_min=pending.score_min,
                )
                pending = ScoreSnapshot()
                iterations.append(it)

    iterations.sort(key=lambda x: x.pass_num)
    return iterations


def read_text(source: TextIO | BinaryIO) -> str:
    if isinstance(source, BinaryIO):
        return source.read().decode("utf-8", errors="replace")
    return source.read()


def write_csv(iterations: List[Iteration], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "pass",
                "grammar_size",
                "rules",
                "bits_per_sym",
                "o0e_bytes",
                "o0e_mb",
                "score_max",
                "score_min",
                "score_candidates",
                "cycle_start",
            ]
        )
        for it in iterations:
            w.writerow(
                [
                    it.pass_num,
                    it.grammar_size,
                    it.rules,
                    it.bits_per_sym if it.bits_per_sym is not None else "",
                    it.o0e_bytes if it.o0e_bytes is not None else "",
                    f"{it.o0e_bytes / 1e6:.6f}" if it.o0e_bytes else "",
                    it.score.score_max if it.score.score_max is not None else "",
                    it.score.score_min if it.score.score_min is not None else "",
                    it.score.candidates if it.score.candidates is not None else "",
                    it.score.cycle_start if it.score.cycle_start is not None else "",
                ]
            )
    print(f"Wrote {path}")


def plot_iterations(
    iterations: List[Iteration], out_dir: Path, title_suffix: str = ""
) -> None:
    import matplotlib.pyplot as plt

    if not iterations:
        print("No iterations parsed — nothing to plot.", file=sys.stderr)
        return

    out_dir.mkdir(parents=True, exist_ok=True)
    passes = [it.pass_num for it in iterations]
    suffix = f" ({title_suffix})" if title_suffix else ""

    def save(fig, name: str) -> None:
        path = out_dir / name
        fig.tight_layout()
        fig.savefig(path, dpi=150)
        plt.close(fig)
        print(f"Wrote {path}")

    gs = [it.grammar_size for it in iterations]
    rules = [it.rules for it in iterations]
    bits = [it.bits_per_sym for it in iterations if it.bits_per_sym is not None]
    bits_passes = [it.pass_num for it in iterations if it.bits_per_sym is not None]
    o0e = [it.o0e_bytes for it in iterations if it.o0e_bytes is not None]
    o0e_passes = [it.pass_num for it in iterations if it.o0e_bytes is not None]

    fig, axes = plt.subplots(2, 2, figsize=(12, 9))
    fig.suptitle(f"GLZA compression passes{suffix}")

    axes[0, 0].plot(passes, gs, "o-", markersize=3, linewidth=1)
    axes[0, 0].set_xlabel("Pass")
    axes[0, 0].set_ylabel("Grammar size (symbols)")
    axes[0, 0].set_title("Grammar size")
    axes[0, 0].grid(True, alpha=0.3)

    axes[0, 1].plot(passes, rules, "o-", color="C1", markersize=3, linewidth=1)
    axes[0, 1].set_xlabel("Pass")
    axes[0, 1].set_ylabel("Rules")
    axes[0, 1].set_title("Production rules")
    axes[0, 1].grid(True, alpha=0.3)

    if bits:
        axes[1, 0].plot(bits_passes, bits, "o-", color="C2", markersize=3, linewidth=1)
    axes[1, 0].set_xlabel("Pass")
    axes[1, 0].set_ylabel("bits/sym")
    axes[1, 0].set_title("Order-0 entropy (bits/symbol)")
    axes[1, 0].grid(True, alpha=0.3)

    if o0e:
        axes[1, 1].plot(o0e_passes, [x / 1e6 for x in o0e], "o-", color="C3", markersize=3, linewidth=1)
        axes[1, 1].set_ylabel("o0e (MB)")
    axes[1, 1].set_xlabel("Pass")
    axes[1, 1].set_title("Order-0 entropy (bytes)")
    axes[1, 1].grid(True, alpha=0.3)

    save(fig, "glza_grammar.png")

    score_iters = [it for it in iterations if it.score.score_max is not None]
    if score_iters:
        fig, ax = plt.subplots(figsize=(10, 5))
        sp = [it.pass_num for it in score_iters]
        hi = [it.score.score_max for it in score_iters]
        lo = [it.score.score_min for it in score_iters]
        ax.fill_between(sp, lo, hi, alpha=0.25, label="score range")
        ax.plot(sp, hi, "o-", label="score max", markersize=3, linewidth=1)
        ax.plot(sp, lo, "o-", label="score min", markersize=3, linewidth=1)
        ax.set_xlabel("Pass")
        ax.set_ylabel("Candidate score")
        ax.set_title(f"Score range per pass{suffix}")
        ax.legend()
        ax.grid(True, alpha=0.3)
        save(fig, "glza_scores.png")

    if len(iterations) >= 2:
        fig, ax = plt.subplots(figsize=(10, 5))
        dp = passes[1:]
        d_gs = [gs[i] - gs[i - 1] for i in range(1, len(gs))]
        d_rules = [rules[i] - rules[i - 1] for i in range(1, len(rules))]
        ax.plot(dp, d_gs, "o-", label="Δ grammar size", markersize=3, linewidth=1)
        ax.plot(dp, d_rules, "o-", label="Δ rules", markersize=3, linewidth=1)
        ax.axhline(0, color="k", linewidth=0.5, alpha=0.5)
        ax.set_xlabel("Pass")
        ax.set_ylabel("Change from previous pass")
        ax.set_title(f"Per-pass deltas{suffix}")
        ax.legend()
        ax.grid(True, alpha=0.3)
        save(fig, "glza_deltas.png")

    last = iterations[-1]
    print(
        f"\nParsed {len(iterations)} passes "
        f"(#{iterations[0].pass_num} … #{last.pass_num})"
    )
    print(
        f"Latest: grammar={last.grammar_size:,} rules={last.rules:,} "
        f"bits/sym={last.bits_per_sym} o0e={last.o0e_bytes}"
    )


def print_table(iterations: List[Iteration], limit: int = 20) -> None:
    if not iterations:
        return
    rows = iterations[-limit:]
    print(
        f"\n{'pass':>6} {'grammar':>14} {'rules':>8} "
        f"{'bits/sym':>10} {'o0e_MB':>12} {'score':>22}"
    )
    print("-" * 78)
    for it in rows:
        o0e = f"{it.o0e_bytes / 1e6:.2f}" if it.o0e_bytes else "—"
        bits = f"{it.bits_per_sym:.4f}" if it.bits_per_sym is not None else "—"
        sc = "—"
        if it.score.score_max is not None:
            sc = f"{it.score.score_max:.3f}-{it.score.score_min:.3f}"
        print(
            f"{it.pass_num:6d} {it.grammar_size:14,d} {it.rules:8,d} "
            f"{bits:>10} {o0e:>12} {sc:>22}"
        )


def _check_matplotlib() -> None:
    try:
        import matplotlib  # noqa: F401
    except ImportError:
        sys.exit(
            "matplotlib is not available in this Python.\n"
            "Use the venv wrapper:\n"
            "  ./scripts/glza_log_plot.sh [options] [logfile]\n"
            "Or:\n"
            "  python3 -m venv scripts/.venv\n"
            "  scripts/.venv/bin/pip install -r scripts/requirements-glza-plot.txt\n"
            "  scripts/.venv/bin/python scripts/glza_log_plot.py ...\n"
        )


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Parse GLZA PRINTON logs and plot iteration statistics (matplotlib).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("logfile", nargs="?", type=Path, help="Log file (default: stdin)")
    ap.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        default=Path("glza_plots"),
        help="Directory for PNG + CSV (default: ./glza_plots)",
    )
    ap.add_argument(
        "--no-plot",
        action="store_true",
        help="Only print table + CSV, skip PNG charts",
    )
    ap.add_argument("--table-rows", type=int, default=15)
    ap.add_argument("--watch", action="store_true")
    ap.add_argument("--watch-interval", type=float, default=30.0)
    args = ap.parse_args()

    if not args.no_plot:
        _check_matplotlib()

    def run_once(text: str, label: str) -> List[Iteration]:
        iterations = parse_lines(text.splitlines())
        print_table(iterations, limit=args.table_rows)
        if iterations:
            write_csv(iterations, args.output_dir / "glza_passes.csv")
        if not args.no_plot and iterations:
            plot_iterations(iterations, args.output_dir, title_suffix=label)
        return iterations

    if args.watch:
        if not args.logfile:
            ap.error("--watch requires a log file path")
        path = args.logfile
        print(f"Watching {path} every {args.watch_interval}s → {args.output_dir}")
        while True:
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
                run_once(text, path.name)
                print(f"({time.strftime('%H:%M:%S')})")
            except KeyboardInterrupt:
                print("\nStopped.")
                break
            time.sleep(args.watch_interval)
        return

    if args.logfile:
        text = args.logfile.read_text(encoding="utf-8", errors="replace")
        label = args.logfile.name
    else:
        text = read_text(sys.stdin)
        label = "stdin"

    run_once(text, label)


if __name__ == "__main__":
    main()
