"""Command line entry point: ``python3 -m wintab``.

    python3 -m wintab                 # switch apps with three fingers
    python3 -m wintab --probe 20      # confirm the pad reports three contacts
    python3 -m wintab --dry-run       # read the pad, print decisions, draw nothing
    python3 -m wintab --replay log.jsonl    # re-run a recorded swipe
    python3 -m wintab --record log.jsonl    # record frames while running
    python3 -m wintab --print-config  # show every tunable and where it comes from
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import sys
import time

from . import __version__, config as config_module
from .app import WinTab, find_conflicts
from .touchpad import ContactFrame, DeviceLost, EvdevSource, find_touchpad


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="wintab",
        description="Windows 11 style three-finger app switching for X11/Cinnamon.",
    )
    parser.add_argument("--config", type=pathlib.Path, help="config file to read")
    parser.add_argument("--device", help="substring of the touchpad's input device name")
    parser.add_argument("--invert", action="store_true", help="swap the swipe direction")
    parser.add_argument("--fps", action="store_true", help="draw a frame-rate readout")
    parser.add_argument("-v", "--verbose", action="store_true", help="print every gesture detail")
    parser.add_argument("--dry-run", action="store_true", help="decide, but never draw or switch")
    parser.add_argument("--probe", nargs="?", const=20.0, type=float, metavar="SECONDS",
                        help="watch the pad and report how many contacts it sees")
    parser.add_argument("--replay", type=pathlib.Path, metavar="FILE",
                        help="replay a recorded JSONL frame log in real time")
    parser.add_argument("--record", type=pathlib.Path, metavar="FILE",
                        help="append every frame to this JSONL file")
    parser.add_argument("--print-config", action="store_true",
                        help="print the effective configuration and exit")
    parser.add_argument("--version", action="version", version=f"wintab {__version__}")
    return parser.parse_args(argv)


def build_config(args: argparse.Namespace):
    cfg = config_module.load(args.config)
    if args.device:
        cfg.touchpad_name = args.device
    if args.invert:
        cfg.invert_direction = not cfg.invert_direction
    if args.fps:
        cfg.show_fps = True
    if args.verbose:
        cfg.verbose = True
    return cfg


def print_config(cfg, path: pathlib.Path | None) -> None:
    source = path or config_module.CONFIG_PATH
    if source.exists():
        print(f"# effective configuration (from {source})")
    else:
        print(f"# built-in defaults (no config file at {source})")
    for field in dataclasses.fields(cfg):
        value = getattr(cfg, field.name)
        if isinstance(value, bool):
            value = "true" if value else "false"
        elif isinstance(value, str):
            value = f'"{value}"'
        print(f"{field.name} = {value}")


def probe(seconds: float, device: str | None) -> int:
    """Live view of the pad: the one thing to run before trusting the gesture."""
    try:
        node, name = find_touchpad(device)
    except RuntimeError as exc:
        print(f"wintab: {exc}")
        print("wintab: is your user in the 'input' group?  groups | grep input")
        return 1

    try:
        source = EvdevSource(node, name)
    except PermissionError:
        print(f"wintab: permission denied opening {node}")
        print("wintab: add yourself to the 'input' group and log back in:")
        print("        sudo usermod -aG input $USER")
        return 1

    geometry = source.geometry
    print(f"wintab: reading {name}")
    print(f"wintab: node {node}  range {geometry.x_min}..{geometry.x_max} x "
          f"{geometry.y_min}..{geometry.y_max}")
    print(f"wintab: watch for {seconds:.0f}s -- put three fingers down and drag left and right")
    print()

    deadline = time.monotonic() + max(1.0, seconds)
    best = 0
    frames = 0
    last_line = ""
    try:
        while time.monotonic() < deadline:
            try:
                batch = source.read_frames(timeout=0.5)
            except DeviceLost as exc:
                print(f"\nwintab: device lost ({exc})")
                break
            for frame in batch:
                frames += 1
                best = max(best, frame.count)
                line = describe(frame)
                if line != last_line:
                    print(f"\r{line:<72}", end="", flush=True)
                    last_line = line
    except KeyboardInterrupt:
        print()
    finally:
        source.close()

    print()
    print()
    print(f"wintab: frames read: {frames}, most contacts seen at once: {best}")
    if best >= 3:
        print("wintab: PASS -- the pad reports three simultaneous contacts.")
        return 0
    print(
        "wintab: FAIL -- fewer than three contacts were seen.\n"
        "wintab: a three-finger gesture cannot work on this pad; check that you\n"
        "wintab: really touched it during the run, and that no other gesture\n"
        "wintab: tool (touchegg) is swallowing the touches first."
    )
    return 1


def describe(frame: ContactFrame) -> str:
    if not frame.contacts:
        return "no contacts"
    spots = "  ".join(f"{c.x:.3f},{c.y:.3f}" for c in frame.contacts)
    return f"{frame.count} contact(s): {spots}"


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    cfg = build_config(args)

    if args.print_config:
        print_config(cfg, args.config)
        return 0

    if args.probe is not None:
        return probe(args.probe, cfg.touchpad_name)

    for warning in find_conflicts():
        print(f"wintab: WARNING: {warning}")

    app = WinTab(
        cfg,
        use_overlay=not args.dry_run,
        record_path=str(args.record) if args.record else None,
    )
    if args.replay:
        app.replay(str(args.replay))
    return app.run()


if __name__ == "__main__":
    sys.exit(main())
