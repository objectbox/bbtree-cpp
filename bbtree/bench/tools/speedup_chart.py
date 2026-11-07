#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib>=3.8"]
# ///
"""Plots maps' speed against a baseline map, a row per bench cell.

Reads a CSV of bench times (ns per operation, smaller is better): a `cell`
column naming the bench cell, a column for the baseline map, and a column for
each map to plot; other text columns are ignored. Each map's dot sits at the
baseline's time divided by the map's, on a log axis centred on 1x: right of
the line, the map is faster than the baseline.

    uv run bbtree/bench/tools/speedup_chart.py bbtree/bench/charts/integer_keys.csv \
        --title "Integer keys" -o bbtree/bench/charts/integer_keys.png
    uv run bbtree/bench/tools/speedup_chart.py ... --svg   # SVG instead of PNG
    uv run bbtree/bench/tools/speedup_chart.py ... --1m    # only the 1M-entry cells

PNGs are 2000 pixels wide.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FixedLocator, NullLocator  # noqa: E402

# The reference palette's light mode (validated: the two series pass the
# lightness, chroma, colour-blind and contrast checks on this surface).
SURFACE = "#fcfcfb"
TEXT_PRIMARY = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
GRIDLINE = "#e1e0d9"
BASELINE_LINE = "#8a8984"
SERIES_COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]

ROW_BAND = "#f4f3ef"
ROW_DODGE = 0.2
HEADER_INCHES = 1.0

PNG_WIDTH_PIXELS = 2000
FIGURE_WIDTH_INCHES = 12.8


@dataclass(frozen=True)
class BenchRow:
    """One bench cell: its label and every map's time on it."""

    label: str
    times: dict[str, float]

    # `u64_u64_random_insert_1k` -> "random insert, 1k";
    # `str-json_struct32_build_many_1Mx4` -> "JSON keys: build many, 1M maps of 4".
    CELL_PATTERN = re.compile(r"^(?P<key>[a-z0-9-]+)_(?P<value>[a-z0-9]+)_(?P<op>.+)_(?P<size>[0-9]+[kM](x[0-9]+)?)$")
    KEY_NAMES = {"u64": "", "str-16": "16-byte keys: ", "str-json": "JSON keys: "}

    @classmethod
    def size_of(cls, cell: str) -> str | None:
        """The cell's size as the bench names it: `1k`, `1M`, `1Mx4`."""
        match = cls.CELL_PATTERN.match(cell)
        return match["size"] if match else None

    @classmethod
    def label_of(cls, cell: str, with_size: bool = True) -> str:
        match = cls.CELL_PATTERN.match(cell)
        if match is None:
            return cell
        key = cls.KEY_NAMES.get(match["key"], f"{match['key']} keys: ")
        label = f"{key}{match['op'].replace('_', ' ')}"
        if not with_size:
            return label
        size = match["size"]
        if "x" in size:
            maps, entries = size.split("x")
            size = f"{maps} maps of {entries}"
        return f"{label}, {size}"


class SpeedupTable:
    """The CSV's rows, and each plotted map's speedup over the baseline."""

    def __init__(self, path: Path, baseline: str, only_size: str | None = None) -> None:
        with path.open(newline="") as file:
            reader = csv.DictReader(file)
            columns = reader.fieldnames or []
            records = list(reader)
        if "cell" not in columns or baseline not in columns:
            raise SystemExit(f"{path}: needs a `cell` and a `{baseline}` column, has {columns}")
        self.baseline = baseline
        self.maps = [c for c in columns if c not in ("cell", baseline) and self._numeric(records, c)]
        if only_size is not None:
            records = [r for r in records if BenchRow.size_of(r["cell"]) == only_size]
            if not records:
                raise SystemExit(f"{path}: no cells of size {only_size}")
        # With one size, the title says it and the rows don't repeat it.
        self.rows = [
            BenchRow(BenchRow.label_of(r["cell"], with_size=only_size is None),
                     {c: float(r[c]) for c in [baseline, *self.maps]})
            for r in records
        ]

    @staticmethod
    def _numeric(records: list[dict[str, str]], column: str) -> bool:
        try:
            for record in records:
                float(record[column])
        except ValueError:
            return False
        return bool(records)

    def speedup(self, row: BenchRow, map_name: str) -> float:
        return row.times[self.baseline] / row.times[map_name]


class SpeedupChart:
    """A dot plot: a row per cell, a dot per map, on a log axis around 1x."""

    def __init__(self, table: SpeedupTable, title: str, subtitle: str) -> None:
        self.table = table
        self.title = title
        self.subtitle = subtitle

    def save(self, path: Path, svg: bool) -> None:
        figure = self._draw()
        if svg:
            figure.savefig(path, format="svg", facecolor=SURFACE)
        else:
            figure.savefig(path, format="png", dpi=PNG_WIDTH_PIXELS / FIGURE_WIDTH_INCHES, facecolor=SURFACE)
        plt.close(figure)

    def _draw(self) -> plt.Figure:
        rows = self.table.rows
        height = HEADER_INCHES + 0.9 + 0.36 * len(rows)
        figure, axes = plt.subplots(figsize=(FIGURE_WIDTH_INCHES, height))
        figure.patch.set_facecolor(SURFACE)
        axes.set_facecolor(SURFACE)

        speedups = [self.table.speedup(r, m) for r in rows for m in self.table.maps]
        low, high = self._axis_range(speedups)
        axes.set_xscale("log")
        axes.set_xlim(low, high)
        axes.set_ylim(len(rows) - 0.5, -0.5)

        # Each map's dots, row by row, joined by a line of its colour; each
        # map's dots sit a little off the rows' centres, so that maps of about
        # the same speed don't hide each other. The first map, the one the
        # chart is about, draws on top.
        maps = self.table.maps
        for index, map_name in enumerate(maps):
            offset = (index - (len(maps) - 1) / 2) * ROW_DODGE
            xs = [self.table.speedup(row, map_name) for row in rows]
            ys = [y + offset for y in range(len(rows))]
            layer = 3 + 2 * (len(maps) - index)
            axes.plot(xs, ys, color=SERIES_COLORS[index], linewidth=2.0, alpha=0.55, zorder=layer,
                      solid_capstyle="round", solid_joinstyle="round")
            axes.scatter(xs, ys, s=90, color=SERIES_COLORS[index], edgecolors=SURFACE, linewidths=1.5,
                         zorder=layer + 1)
        for y in range(1, len(rows), 2):
            axes.axhspan(y - 0.5, y + 0.5, color=ROW_BAND, zorder=0, linewidth=0)

        # The baseline: 1x, labelled at the top.
        axes.axvline(1.0, color=BASELINE_LINE, linewidth=1.2, zorder=2)
        axes.text(1.0, -0.75, f"{self.table.baseline} = 1×", ha="center", va="bottom", fontsize=10,
                  color=TEXT_SECONDARY, transform=axes.transData)

        ticks = [t for t in (0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256) if low <= t <= high]
        axes.xaxis.set_major_locator(FixedLocator(ticks))
        axes.xaxis.set_minor_locator(NullLocator())
        axes.set_xticklabels([f"{t:g}×" for t in ticks])
        axes.grid(axis="x", color=GRIDLINE, linewidth=0.8, zorder=0)
        axes.set_yticks(range(len(rows)))
        axes.set_yticklabels([r.label for r in rows])
        axes.tick_params(axis="both", length=0, labelsize=11, colors=TEXT_PRIMARY)
        for spine in axes.spines.values():
            spine.set_visible(False)
        axes.set_xlabel(f"← slower than {self.table.baseline}      speedup, log scale      faster than "
                        f"{self.table.baseline} →", fontsize=11, color=TEXT_SECONDARY, labelpad=10)

        handles = [
            axes.scatter([], [], s=90, color=SERIES_COLORS[i], edgecolors=SURFACE, linewidths=1.5, label=m)
            for i, m in enumerate(self.table.maps)
        ]
        figure.text(0.01, 1.0 - 0.2 / height, self.title, ha="left", va="top", fontsize=16, fontweight="bold",
                    color=TEXT_PRIMARY)
        figure.text(0.01, 1.0 - 0.58 / height, self.subtitle, ha="left", va="top", fontsize=11,
                    color=TEXT_SECONDARY)
        figure.legend(handles=handles, loc="upper left", bbox_to_anchor=(0.0, 1.0 - 0.82 / height), ncol=len(handles),
                      frameon=False, fontsize=11, labelcolor=TEXT_PRIMARY, borderaxespad=0.0,
                      handletextpad=0.3, columnspacing=1.6)
        figure.tight_layout(rect=(0, 0, 1, 1.0 - HEADER_INCHES / height))
        return figure

    @staticmethod
    def _axis_range(speedups: list[float]) -> tuple[float, float]:
        low = min(min(speedups), 1.0)
        high = max(max(speedups), 1.0)
        return 2 ** (math.floor(math.log2(low)) - 0.3), 2 ** (math.ceil(math.log2(high)) + 0.3)


class CommandLine:
    """Arguments: the CSV, where to write, and the chart's words."""

    @staticmethod
    def run() -> None:
        parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
        parser.add_argument("csv", type=Path, help="bench times: a `cell` column and a column per map")
        parser.add_argument("-o", "--output", type=Path, help="output file (default: the CSV's name, .png or .svg)")
        parser.add_argument("--svg", action="store_true", help="write SVG instead of a 2000-pixel-wide PNG")
        parser.add_argument("--baseline", default="std::map", help="the column every map is compared with")
        parser.add_argument("--title", default="", help="the chart's title")
        parser.add_argument("--subtitle", default=None, help="the line under the title")
        parser.add_argument("--1m", dest="only_1m", action="store_true",
                            help="only the cells of one map of 1M entries (default output: the CSV's name + _1m)")
        arguments = parser.parse_args()

        table = SpeedupTable(arguments.csv, arguments.baseline, "1M" if arguments.only_1m else None)
        subtitle = arguments.subtitle
        if subtitle is None:
            subtitle = f"Speedup over {table.baseline} (its time ÷ the map's), per bench cell; right of 1× is faster"
        stem = arguments.csv.stem + ("_1m" if arguments.only_1m else "")
        output = arguments.output or arguments.csv.with_name(stem + (".svg" if arguments.svg else ".png"))
        SpeedupChart(table, arguments.title or arguments.csv.stem.replace("_", " "), subtitle).save(output,
                                                                                                    arguments.svg)
        print(output)


if __name__ == "__main__":
    CommandLine.run()
