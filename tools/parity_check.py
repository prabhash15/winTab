#!/usr/bin/env python3
"""Differential check: the Python and C++ gesture engines on the same input.

Synthesises touchpad streams (valid swipes, late landings, four fingers, taps,
dropped frames, jitter, pauses), replays each through both engines with the same
configuration, and diffs the event traces. Any divergence is printed with the
recording that reproduces it.

    python3 tools/parity_check.py --cxx build/trace_replay
    python3 tools/parity_check.py --cxx build/trace_replay --count 500

Exit code 0 when every stream matches, 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from wintab.config import load as load_config  # noqa: E402
from wintab.gesture import GestureParams, ThreeFingerSwipe  # noqa: E402
from wintab.touchpad import Contact, ContactFrame  # noqa: E402


def fmt(value: float) -> str:
    return "%.17g" % value


def trace_python(frames: list[dict], params: GestureParams) -> list[str]:
    engine = ThreeFingerSwipe(params)
    lines: list[str] = []
    for record in frames:
        frame = ContactFrame(
            float(record["t"]),
            tuple(Contact(int(s), float(x), float(y)) for s, x, y in record["c"]),
            bool(record.get("dropped", False)),
        )
        for event in engine.feed(frame):
            name = type(event).__name__
            if name == "SwipeBegin":
                lines.append("B " + fmt(event.time))
            elif name == "SwipeUpdate":
                lines.append("U %s %d %s" % (fmt(event.progress), event.steps, fmt(event.time)))
            elif name == "SwipeEnd":
                lines.append(
                    "E %s %s %d %s"
                    % (fmt(event.progress), fmt(event.velocity), event.steps, fmt(event.time))
                )
            elif name == "SwipeCancel":
                lines.append("C %s %s" % (event.reason, fmt(event.time)))
            else:
                lines.append("T " + fmt(event.time))
    return lines


def frame_record(t: float, contacts: list[list], dropped: bool = False) -> dict:
    return {"t": round(t, 6), "c": contacts, "dropped": dropped}


def spread(dx: float, dy: float, count: int = 3, start: float = 0.35) -> list[list]:
    return [
        [index, round(start + 0.15 * index + dx, 5), round(0.5 + dy, 5)]
        for index in range(count)
    ]


def gen_stream(rng: random.Random) -> list[dict]:
    mode = rng.choices(
        ["swipe", "late", "four", "tap", "jitter", "drop"],
        weights=[45, 10, 8, 10, 22, 5],
    )[0]
    frames: list[dict] = []
    t = 0.0

    if mode == "late":
        frames.append(frame_record(t, spread(0.0, 0.0, 1)))
        t += rng.uniform(0.02, 0.2)
        frames.append(frame_record(t, spread(0.0, 0.0)))
        for _ in range(rng.randrange(0, 20)):
            t += 0.0125
            frames.append(frame_record(t, spread(rng.uniform(-0.05, 0.05), 0.0)))
        frames.append(frame_record(t + 0.02, []))
        return frames

    if mode == "four":
        frames.append(frame_record(t, spread(0.0, 0.0, 4)))
        for _ in range(rng.randrange(1, 25)):
            t += 0.0125
            frames.append(frame_record(t, spread(rng.uniform(-0.1, 0.1), 0.0, 4)))
        frames.append(frame_record(t + 0.02, []))
        return frames

    if mode == "tap":
        frames.append(frame_record(t, spread(0.0, 0.0)))
        t += rng.uniform(0.03, 0.4)
        frames.append(frame_record(t, []))
        return frames

    # swipe / jitter / drop: fingers land together, then move.
    frames.append(frame_record(t, spread(0.0, 0.0)))
    dx = 0.0
    dy = 0.0
    steps = rng.randrange(5, 70)
    for _ in range(steps):
        t += rng.choice([0.008, 0.0125, 0.016, 0.02])
        dx += rng.gauss(0.003, 0.010)
        if rng.random() < 0.12:
            dx = 0.0  # jitter back toward the anchor
        if rng.random() < 0.10:
            t += rng.uniform(0.1, 0.5)  # pause: velocity window empties
        if mode == "jitter":
            dy = rng.gauss(0.0, 0.02)
            if rng.random() < 0.10:
                dy = rng.gauss(0.0, 0.15)  # occasional vertical lurch
        if mode == "drop" and rng.random() < 0.08:
            frames.append(frame_record(t, [], dropped=True))
            continue
        frames.append(frame_record(t, spread(max(-0.4, min(0.4, dx)), dy)))
        if rng.random() < 0.08:
            t += 0.0125
            frames.append(frame_record(t, spread(max(-0.4, min(0.4, dx)), dy)))
    frames.append(frame_record(t + 0.02, []))
    return frames


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="build/trace_replay", help="path to trace_replay")
    parser.add_argument("--count", type=int, default=200, help="number of streams")
    parser.add_argument("--config", type=pathlib.Path, default=None, help="config file for both")
    parser.add_argument("--seed", type=int, default=1234)
    args = parser.parse_args()

    config = load_config(args.config)
    params = GestureParams.from_config(config)
    rng = random.Random(args.seed)
    total_events = 0
    failures = 0

    with tempfile.TemporaryDirectory() as directory:
        workspace = pathlib.Path(directory)
        for index in range(args.count):
            frames = gen_stream(rng)
            recording = workspace / f"stream-{index}.jsonl"
            recording.write_text("\n".join(json.dumps(f) for f in frames) + "\n")

            expected = trace_python(frames, params)
            command = [args.cxx, str(recording)]
            if args.config:
                command = [args.cxx, "--config", str(args.config), str(recording)]
            result = subprocess.run(command, capture_output=True, text=True)
            # The config loader reports warnings with the app's own print() (so
            # they land on stdout, as the Python build does); drop those lines
            # and keep every event line strictly comparable.
            actual = [line for line in result.stdout.splitlines() if not line.startswith("wintab: ")]
            total_events += len(expected)
            if result.returncode != 0 or actual != expected:
                failures += 1
                print(f"MISMATCH on {recording} (kept for inspection)")
                for number, (left, right) in enumerate(zip(expected, actual)):
                    if left != right:
                        print(f"  first difference at event {number}:\n    python: {left}\n    c++:    {right}")
                        break
                else:
                    print(f"  python {len(expected)} events, c++ {len(actual)}: {result.stderr}")
                kept = pathlib.Path("/tmp") / f"wintab-parity-{index}.jsonl"
                kept.write_text(recording.read_text())
                print(f"  copied to {kept}")
                if failures >= 3:
                    break

    if failures:
        print(f"FAIL: {failures} of {args.count} stream(s) diverged")
        return 1
    print(f"OK: {args.count} stream(s), {total_events} events, identical traces")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
