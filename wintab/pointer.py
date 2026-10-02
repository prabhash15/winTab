"""Optional: stop the mouse pointer wandering while three fingers swipe.

libinput usually ignores extra fingers for pointer motion, but some pads (and
some libinput versions) still nudge the cursor during a three-finger drag. When
``freeze_pointer`` is enabled we hold an X pointer grab for the duration of the
gesture, confined to a 1x1 window at wherever the cursor happened to be.

Details that matter:

* the grab lives on its **own X connection**. A grab with ``event_mask=0`` and
  ``owner_events=False`` makes X discard every pointer event instead of
  delivering it, so nothing piles up in an unread queue -- but keeping it on a
  separate connection means an accidental unread queue could never stall the
  window manager connection either.
* the confine window is a tiny override-redirect InputOutput window, mapped
  just before the grab (X requires the confine window to be viewable).
* a grab is released by the X server if this process dies, so a crash cannot
  leave the user with a frozen mouse. The app also keeps a watchdog.
"""

from __future__ import annotations

from Xlib import X, display as xdisplay
from Xlib.error import XError


class PointerLock:
    def __init__(self) -> None:
        self._display = None
        self._window = None
        self._grabbed = False
        self._error: str | None = None

    @property
    def grabbed(self) -> bool:
        return self._grabbed

    @property
    def error(self) -> str | None:
        return self._error

    def grab(self) -> bool:
        """Confine the pointer to where it is now. Safe to call repeatedly."""
        if self._grabbed:
            return True
        try:
            display = xdisplay.Display()
            display.set_error_handler(lambda *_args: None)
            root = display.screen().root
            pointer = root.query_pointer()
            window = root.create_window(
                pointer.root_x, pointer.root_y, 1, 1, 0,
                X.CopyFromParent, X.InputOutput, X.CopyFromParent,
                override_redirect=True, event_mask=0,
            )
            window.map()
            display.flush()
            reply = root.grab_pointer(
                False,                    # owner_events: report to the grab window
                0,                        # event_mask: discard, deliver nothing
                X.GrabModeAsync,
                X.GrabModeAsync,
                window,                   # confine_to
                X.NONE,
                X.CurrentTime,
            )
            display.flush()
        except (XError, OSError, AttributeError) as exc:
            self._error = str(exc)
            self.release()
            return False

        status = getattr(reply, "status", X.GrabSuccess)
        if status != X.GrabSuccess:
            self._error = f"grab refused (status {status})"
            try:
                window.destroy()
                display.close()
            except Exception:
                pass
            return False

        self._display = display
        self._window = window
        self._grabbed = True
        self._error = None
        return True

    def release(self) -> None:
        display, self._display = self._display, None
        window, self._window = self._window, None
        grabbed, self._grabbed = self._grabbed, False
        if display is None:
            return
        try:
            if grabbed:
                display.ungrab_pointer(X.CurrentTime)
            if window is not None:
                window.destroy()
            display.flush()
        except Exception:
            pass
        finally:
            try:
                display.close()
            except Exception:
                pass
