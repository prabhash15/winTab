"""Read raw multitouch contacts straight from the touchpad's evdev node.

Why raw evdev instead of libinput/touchegg: libinput hands out already-parsed
*gesture* events, which is exactly what we do not want. To make a window follow
the fingers 1:1 we need the individual contact positions, and we need them at
the device's own report rate (~100-140Hz).

Reading /dev/input/event* needs either root or membership of the `input` group.
The Mint desktop user is normally in that group, so no privilege escalation and
no helper daemon are required.
"""

from __future__ import annotations

import dataclasses
import errno
import fcntl
import math
import os
import pathlib
import re
import select
import struct
import threading
import time
from collections.abc import Callable, Iterable

# --- kernel constants (kept local so we do not need python-evdev) ----------
EV_SYN = 0x00
EV_KEY = 0x01
EV_ABS = 0x03

SYN_REPORT = 0x00
SYN_DROPPED = 0x03

ABS_X = 0x00
ABS_Y = 0x01
ABS_MT_SLOT = 0x2F
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_TRACKING_ID = 0x39

BTN_TOOL_FINGER = 0x145
BTN_TOOL_DOUBLETAP = 0x14D
BTN_TOOL_TRIPLETAP = 0x14E
BTN_TOOL_QUADTAP = 0x14F
BTN_TOOL_QUINTTAP = 0x148

_BTN_TO_FINGERS = {
    BTN_TOOL_FINGER: 1,
    BTN_TOOL_DOUBLETAP: 2,
    BTN_TOOL_TRIPLETAP: 3,
    BTN_TOOL_QUADTAP: 4,
    BTN_TOOL_QUINTTAP: 5,
}

_EVENT = struct.Struct("llHHi")
EVENT_SIZE = _EVENT.size  # 24 bytes on 64-bit

INPUT_DEVICES = pathlib.Path("/proc/bus/input/devices")


@dataclasses.dataclass(frozen=True)
class PadGeometry:
    """Raw ABS ranges of the touchpad, used to normalise positions to 0..1."""

    x_min: int = 0
    x_max: int = 1
    y_min: int = 0
    y_max: int = 1

    @property
    def units_per_mm(self) -> float:
        """Approximate, only used for human-readable messages."""
        return (self.x_max - self.x_min) / 128.0

    def normalise(self, x: int, y: int) -> tuple[float, float]:
        nx = (x - self.x_min) / max(1, self.x_max - self.x_min)
        ny = (y - self.y_min) / max(1, self.y_max - self.y_min)
        return (min(1.0, max(0.0, nx)), min(1.0, max(0.0, ny)))


@dataclasses.dataclass(frozen=True)
class Contact:
    """A single finger, in normalised pad coordinates."""

    slot: int
    x: float
    y: float


@dataclasses.dataclass(frozen=True)
class ContactFrame:
    """All contacts present at one SYN_REPORT, plus a monotonic timestamp."""

    time: float
    contacts: tuple[Contact, ...] = ()
    dropped: bool = False
    btn_fingers: int = 0

    @property
    def count(self) -> int:
        return len(self.contacts)

    @property
    def centroid(self) -> tuple[float, float] | None:
        if not self.contacts:
            return None
        n = len(self.contacts)
        return (
            sum(c.x for c in self.contacts) / n,
            sum(c.y for c in self.contacts) / n,
        )


# ---------------------------------------------------------------------------
# Device discovery
# ---------------------------------------------------------------------------


def _parse_input_devices(text: str) -> list[dict[str, str]]:
    blocks: list[dict[str, str]] = []
    current: dict[str, str] = {}
    for line in text.splitlines():
        if not line.strip():
            if current:
                blocks.append(current)
                current = {}
            continue
        key, _, value = line.partition(":")
        current[key.strip()] = value.strip()
    if current:
        blocks.append(current)
    return blocks


def find_touchpad(name: str | None = None) -> tuple[str, str]:
    """Return (event node path, device name) for the touchpad.

    Prefers the configured `name` substring, otherwise anything whose name
    contains "touchpad" that exposes an event node.
    """
    try:
        text = INPUT_DEVICES.read_text()
    except OSError as exc:
        raise RuntimeError(f"cannot read {INPUT_DEVICES}: {exc}") from exc

    fallback: tuple[str, str] | None = None
    for block in _parse_input_devices(text):
        dev_name = block.get("N", "")
        dev_name = dev_name.removeprefix('Name="').removesuffix('"')
        handlers = block.get("H", "")
        match = re.search(r"\bevent(\d+)\b", handlers)
        if not match:
            continue
        node = f"/dev/input/event{match.group(1)}"
        if name:
            if name.lower() in dev_name.lower():
                return node, dev_name
            continue
        if "touchpad" in dev_name.lower() and fallback is None:
            fallback = (node, dev_name)
    if fallback:
        return fallback
    if name:
        raise RuntimeError(f"no input device matching {name!r} found")
    raise RuntimeError("no touchpad found in /proc/bus/input/devices")


def _eviocgabs(code: int) -> int:
    return 0x80000000 | (24 << 16) | (ord("E") << 8) | (0x40 + code)


def read_absinfo(fd: int, code: int) -> tuple[int, int, int, int, int, int]:
    """EVIOCGABS: (value, min, max, fuzz, flat, resolution)."""
    request = _eviocgabs(code)
    try:
        buf = fcntl.ioctl(fd, request, b"\0" * 24)
    except OverflowError:  # pragma: no cover - some Python builds want signed
        request = struct.unpack("i", struct.pack("I", request & 0xFFFFFFFF))[0]
        buf = fcntl.ioctl(fd, request, b"\0" * 24)
    return struct.unpack("6i", buf)


def read_geometry(fd: int) -> PadGeometry:
    try:
        _, x_min, x_max, _, _, _ = read_absinfo(fd, ABS_MT_POSITION_X)
        _, y_min, y_max, _, _, _ = read_absinfo(fd, ABS_MT_POSITION_Y)
    except OSError:
        _, x_min, x_max, _, _, _ = read_absinfo(fd, ABS_X)
        _, y_min, y_max, _, _, _ = read_absinfo(fd, ABS_Y)
    if x_max <= x_min:
        x_min, x_max = 0, 1
    if y_max <= y_min:
        y_min, y_max = 0, 1
    return PadGeometry(x_min, x_max, y_min, y_max)


# ---------------------------------------------------------------------------
# Frame sources
# ---------------------------------------------------------------------------


class EvdevSource:
    """Blocking-ish source reading an evdev touchpad node."""

    def __init__(self, node: str, name: str = "", exclusive: bool = False):
        self.node = node
        self.name = name
        flags = os.O_RDONLY
        if not exclusive:
            flags |= os.O_NONBLOCK
        self.fd = os.open(node, flags)
        if exclusive:
            os.set_blocking(self.fd, False)
        self.geometry = read_geometry(self.fd)
        self._slots: dict[int, list[float | int]] = {}
        self._current_slot = 0
        self._btn_fingers = 0
        self._buf = b""
        self._closed = False

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        if not self._closed:
            self._closed = True
            try:
                os.close(self.fd)
            except OSError:
                pass

    @property
    def closed(self) -> bool:
        return self._closed

    def fileno(self) -> int:
        return self.fd

    # -- decoding ----------------------------------------------------------
    def _decode(self, data: bytes) -> list[ContactFrame]:
        frames: list[ContactFrame] = []
        now = time.monotonic()
        self._buf += data
        usable = len(self._buf) - (len(self._buf) % EVENT_SIZE)
        for offset in range(0, usable, EVENT_SIZE):
            _, _, etype, code, value = _EVENT.unpack_from(self._buf, offset)
            if etype == EV_ABS:
                self._abs(code, value)
            elif etype == EV_KEY:
                if code in _BTN_TO_FINGERS:
                    self._btn_fingers = _BTN_TO_FINGERS[code] if value else 0
            elif etype == EV_SYN:
                if code == SYN_REPORT:
                    frames.append(self._frame(now))
                elif code == SYN_DROPPED:
                    self._slots.clear()
                    frames.append(ContactFrame(now, (), dropped=True))
        self._buf = self._buf[usable:]
        return frames

    def _abs(self, code: int, value: int) -> None:
        if code == ABS_MT_SLOT:
            self._current_slot = value
            return
        slot = self._slots.setdefault(self._current_slot, [None, 0, 0])
        if code == ABS_MT_TRACKING_ID:
            if value == -1:
                self._slots.pop(self._current_slot, None)
            else:
                slot[0] = value
        elif code == ABS_MT_POSITION_X:
            slot[1] = value
        elif code == ABS_MT_POSITION_Y:
            slot[2] = value
        elif code == ABS_X and self._current_slot not in self._slots:
            slot[1] = value
        elif code == ABS_Y and self._current_slot not in self._slots:
            slot[2] = value

    def _frame(self, now: float) -> ContactFrame:
        contacts = []
        for slot, (tracking, x, y) in sorted(self._slots.items()):
            if tracking is None:
                continue
            nx, ny = self.geometry.normalise(int(x), int(y))
            contacts.append(Contact(slot, nx, ny))
        return ContactFrame(now, tuple(contacts), btn_fingers=self._btn_fingers)

    def read_frames(self, timeout: float = 0.5) -> list[ContactFrame]:
        """Wait for input, then return every frame that arrived."""
        if self._closed:
            return []
        try:
            ready, _, _ = select.select([self.fd], [], [], timeout)
        except (OSError, ValueError):
            return []
        if not ready:
            return []
        frames: list[ContactFrame] = []
        while True:
            try:
                data = os.read(self.fd, EVENT_SIZE * 128)
            except BlockingIOError:
                break
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    break
                if exc.errno in (errno.ENODEV, errno.EIO):
                    raise DeviceLost(str(exc)) from exc
                raise
            if not data:
                break
            frames.extend(self._decode(data))
            # Drain whatever else is pending, but never block a second time:
            # one select() per batch keeps latency low at ~140Hz input rates.
            try:
                ready, _, _ = select.select([self.fd], [], [], 0)
            except (OSError, ValueError):
                break
            if not ready:
                break
        return frames


class DeviceLost(RuntimeError):
    """The touchpad node disappeared (suspend, unplug, driver reload)."""


# ---------------------------------------------------------------------------
# Thread plumbing
# ---------------------------------------------------------------------------


class TouchpadReader:
    """Runs an EvdevSource in a thread and hands frames to a callback.

    The callback runs on the reader thread; callers are responsible for
    hopping to their own thread if they need to.
    """

    def __init__(
        self,
        on_frames: Callable[[list[ContactFrame]], None],
        name: str | None = None,
        on_device_change: Callable[[str, PadGeometry | None], None] | None = None,
        reconnect_delay: float = 2.0,
    ):
        self._on_frames = on_frames
        self._name_filter = name
        self._on_device_change = on_device_change
        self._reconnect_delay = reconnect_delay
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self.source: EvdevSource | None = None

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="wintab-touchpad", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1.5)

    @property
    def running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    # -- thread body -------------------------------------------------------
    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                node, dev_name = find_touchpad(self._name_filter)
                source = EvdevSource(node, dev_name)
            except (RuntimeError, OSError) as exc:
                self._report(f"touchpad unavailable ({exc}); retrying", None)
                if self._stop.wait(self._reconnect_delay):
                    return
                continue

            self.source = source
            self._report(f"reading {dev_name} at {node}", source.geometry)
            try:
                while not self._stop.is_set():
                    frames = source.read_frames(timeout=0.5)
                    if frames:
                        try:
                            self._on_frames(frames)
                        except Exception as exc:  # never kill the reader
                            self._report(f"frame handler error: {exc!r}", None)
            except DeviceLost as exc:
                self._report(f"device lost ({exc}); reconnecting", None)
            finally:
                source.close()
                self.source = None
            if self._stop.wait(self._reconnect_delay):
                return

    def _report(self, message: str, geometry: PadGeometry | None) -> None:
        if self._on_device_change:
            try:
                self._on_device_change(message, geometry)
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Replay source (for tests, tuning and --replay)
# ---------------------------------------------------------------------------


def iter_replay_frames(lines: Iterable[str]) -> Iterable[ContactFrame]:
    """Yield ContactFrames with real time offsets from a recorded JSONL log."""
    import json

    previous: float | None = None
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        record = json.loads(line)
        stamp = float(record["t"])
        if previous is not None:
            delay = stamp - previous
            if delay > 0:
                time.sleep(min(delay, 1.0))
        previous = stamp
        contacts = tuple(Contact(int(s), float(x), float(y)) for s, x, y in record["c"])
        yield ContactFrame(stamp, contacts, bool(record.get("dropped", False)))


def format_frames(frames: Iterable[ContactFrame]) -> str:
    import json

    out = []
    for frame in frames:
        out.append(
            json.dumps(
                {
                    "t": round(frame.time, 6),
                    "c": [[c.slot, round(c.x, 5), round(c.y, 5)] for c in frame.contacts],
                    "dropped": frame.dropped,
                }
            )
        )
    return "\n".join(out)


def distance(a: tuple[float, float], b: tuple[float, float]) -> float:
    return math.hypot(a[0] - b[0], a[1] - b[1])
