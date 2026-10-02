#!/usr/bin/env python3
"""Live touchpad probe: confirms the pad reports the three contacts we need.

Run it, put three fingers on the touchpad and drag left and right. The summary
at the end prints the maximum number of simultaneous contacts seen -- the
3-finger gesture needs at least 3.

    python3 tools/probe.py                  # watch for 15 seconds
    python3 tools/probe.py -s 30            # watch for 30 seconds
    python3 tools/probe.py --record log.jsonl   # save frames for --replay

Exit code is 0 when >=3 contacts were seen, 1 otherwise, so it can be used as a
gate in scripts.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from wintab.touchpad import (  # noqa: E402
    ContactFrame,
    DeviceLost,
    EvdevSource,
    find_touchpad,
    format_frames,
)


def describe(frame: ContactFrame) -> str:
    spots = "  ".join(f"{c.x:.3f},{c.y:.3f}" for c in frame.contacts)
    return f"{frame.count} contact(s): {spots}" if frame.contacts else "no contacts"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-s", "--seconds", type=float, default=15.0, help="how long to watch")
    parser.add_argument("-n", "--name", help="substring of the input device name")
    parser.add_argument("--record", type=pathlib.Path, help="append every frame to this JSONL file")
    parser.add_argument("--quiet", action="store_true", help="only print the summary")
    args = parser.parse_args(argv)

    try:
        node, name = find_touchpad(args.name)
    except RuntimeError as exc:
        print(f"probe: {exc}")
        print("probe: is your user in the 'input' group?  groups | grep input")
        return 1

    try:
        source = EvdevSource(node)
    except PermissionError:
        print(f"probe: permission denied opening {node}")
        print("probe: add yourself to the 'input' group and log back in:")
        print("       sudo usermod -aG input $USER")
        return 1
    except OSError as exc:
        print(f"probe: cannot open {node}: {exc}")
        return 1

    geo = source.geometry
    print(f"device : {name}")
    print(f"node   : {node}")
    print(f"range  : x {geo.x_min}..{geo.x_max}   y {geo.y_min}..{geo.y_max}")
    print(f"watch  : {args.seconds:.0f}s")
    print("touch the pad with three fingers and drag left/right...")
    print()

    record = args.record.open("a") if args.record else None
    max_contacts = 0
    seen_counts: dict[int, int] = {}
    last_line = 0.0
    started = time.monotonic()
    frames_seen = 0
    try:
        while time.monotonic() - started < args.seconds:
            for frame in source.read_frames(timeout=0.25):
                frames_seen += 1
                if record is not None:
                    record.write(format_frames([frame]) + "\n")
                if frame.count:
                    max_contacts = max(max_contacts, frame.count)
                    seen_counts[frame.count] = seen_counts.get(frame.count, 0) + 1
                    if not args.quiet and time.monotonic() - last_line > 0.1:
                        last_line = time.monotonic()
                        elapsed = time.monotonic() - started
                        print(f"\r[{elapsed:5.1f}s] btn={frame.btn_fingers} {describe(frame):<70}", end="")
    except KeyboardInterrupt:
        print()
    except DeviceLost:
        print("\nprobe: device disappeared (suspend or driver reload?)")
    finally:
        source.close()
        if record is not None:
            record.close()

    if not args.quiet:
        print()
    print()
    print(f"frames read          : {frames_seen}")
    print(f"max simultaneous     : {max_contacts}")
    if seen_counts:
        counts = ", ".join(f"{n} finger(s) x{c}" for n, c in sorted(seen_counts.items()))
        print(f"contact counts seen  : {counts}")
    if max_contacts >= 3:
        print("gate                 : PASS - the pad reports 3+ contacts")
        return 0
    if max_contacts == 0:
        print("gate                 : INCONCLUSIVE - no contacts were seen at all")
        print("                       did you touch the pad while it was running?")
        return 1
    print(f"gate                 : FAIL - only {max_contacts} contact(s) reported")
    print("                       this pad cannot drive a 3-finger gesture")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
