"""Gamepad control of the desktop launcher: SDL3's gamepad API read through ctypes, turned into menu actions.

Qt 6 has no gamepad module, so the launcher reads pads itself. `PadReader` is pure logic (button edges, the
left stick as a d-pad, held-direction repeat); `Sdl3Backend` feeds it from libSDL3, the library the emulator's
Android window already needs. `connect` attaches it to the library window, which sees ordinary key events: the
pad moves focus, opens, backs out and presses buttons the way a keyboard does, in dialogs and menus too.

Only the window in front reacts. A game started from the launcher reads the same pad, so a button pressed
there must not also launch, stop or uninstall something here: while no launcher window is active the reader is
held back, and a button still down when the window returns is ignored until it is released.
"""
import ctypes
import os
from pathlib import Path
import sys
import time
import traceback

from android_paths import ROOT, installed

# SDL_gamepad.h / SDL_init.h (SDL 3), checked against the 3.4.x headers.
INIT_GAMEPAD = 0x2000
SOUTH, EAST, WEST, NORTH, BACK, GUIDE, START = 0, 1, 2, 3, 4, 5, 6
LEFT_SHOULDER, RIGHT_SHOULDER = 9, 10
DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT = 11, 12, 13, 14
AXIS_LEFTX, AXIS_LEFTY = 0, 1
LABELS = {1: "A", 2: "B", 3: "X", 4: "Y", 5: "Cross", 6: "Circle", 7: "Square", 8: "Triangle"}

BUTTON_ACTIONS = {SOUTH: "accept", EAST: "back", WEST: "sort", NORTH: "menu", START: "options", BACK: "details",
                  LEFT_SHOULDER: "page_up", RIGHT_SHOULDER: "page_down",
                  DPAD_UP: "up", DPAD_DOWN: "down", DPAD_LEFT: "left", DPAD_RIGHT: "right"}
REPEATING = ("up", "down", "left", "right", "page_up", "page_down")
STICK_PRESS, STICK_RELEASE = 0.55, 0.35  # a stick leans past the first to press a direction and back under the second to let go
REPEAT_DELAY, REPEAT_INTERVAL = 0.4, 0.11


class PadReader:
    """Actions from pad states. `backend.read()` lists one dict per connected pad:
    {"name": str, "buttons": set of SDL_GamepadButton numbers held, "axes": {axis: -1.0..1.0}, "labels": {button: label}}."""

    def __init__(self, backend, clock=time.monotonic):
        self.backend, self.clock = backend, clock
        self.held = {}        # action -> time it next repeats, for the actions held down
        self.seen = set()     # actions that were down at the previous poll
        self.stick = None     # the direction the left stick is holding, with hysteresis
        self.baseline = True  # the next poll only records what is already held
        self.labels, self.connected, self.name = {}, False, ""

    def reset(self):
        """Forget what is held; whatever is down now is ignored until it is released."""
        self.held, self.seen, self.stick, self.baseline = {}, set(), None, True

    def down(self, pads):
        """The actions held right now across every pad."""
        actions = set()
        for pad in pads:
            actions |= {BUTTON_ACTIONS[button] for button in pad["buttons"] if button in BUTTON_ACTIONS}
            x, y = pad["axes"].get(AXIS_LEFTX, 0.0), pad["axes"].get(AXIS_LEFTY, 0.0)
            lean = max(abs(x), abs(y))
            if lean >= STICK_PRESS or (self.stick and lean >= STICK_RELEASE):
                horizontal = abs(x) >= abs(y)
                self.stick = ("right" if x > 0 else "left") if horizontal else ("down" if y > 0 else "up")
                actions.add(self.stick)
            else:
                self.stick = None
        return actions

    def poll(self):
        """The actions that begin now, plus held directions as they repeat, in a stable order."""
        pads = self.backend.read()
        self.labels, self.connected = (pads[0]["labels"] if pads else {}), bool(pads)
        self.name = pads[0]["name"] if pads else ""
        now, down = self.clock(), self.down(pads)
        if self.baseline:
            self.baseline, self.seen = False, down
            return []
        fired = [action for action in sorted(down - self.seen)]
        for action in fired:
            if action in REPEATING: self.held[action] = now + REPEAT_DELAY
        for action in list(self.held):
            if action not in down: del self.held[action]
            elif action not in fired and now >= self.held[action]:
                fired.append(action); self.held[action] = now + REPEAT_INTERVAL
        self.seen = down
        return fired

    def label(self, button, default):
        """What the pad calls a button: "A" on an Xbox pad, "Cross" on a PlayStation one."""
        return LABELS.get(self.labels.get(button), default)


def keys_for(action, kind, modal):
    """Key names a pad action presses, given the kind of widget that has the focus: "card", "menu" (a menu or a list
    in a popup), "press" (button, check box, combo box), "edge" (a table or list whose cursor is already at the end the
    action points to) or "other"; `modal` is True in a dialog."""
    arrows = {"up": "Up", "down": "Down", "left": "Left", "right": "Right"}
    if action in arrows:
        # Outside the card grid, Up and Down walk the dialog's focus order, as Tab does; Left and Right stay arrows.
        if modal and kind not in ("card", "menu") and action in ("up", "down"):
            return ["Backtab" if action == "up" else "Tab"]
        return [arrows[action]]
    if action == "accept":
        return ["Space"] if kind == "press" else ["Return"]
    if action == "back":
        return ["Escape"]
    if action == "menu":
        return ["Menu"] if kind == "card" else []
    if action in ("page_up", "page_down"):
        return [("Ctrl+Backtab" if action == "page_up" else "Ctrl+Tab")] if modal else []   # QTabWidget's own keys
    return []


def find_sdl3():
    # The copy `make bootstrap` builds where the system has none: <prefix>/lib/bifrost beside an installed tree
    # (<prefix>/share/bifrost/asgard), else the checkout's Tools/sdl3.
    built = ROOT.parents[2] / "lib/bifrost" if installed() else ROOT / "Tools/sdl3/lib"
    for name in ("libSDL3.so.0", str(built / "libSDL3.so.0")):
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue
    return None


class Sdl3Backend:
    """Connected pads read from libSDL3 (gamepad subsystem only: no window, no video). Raises OSError without SDL3."""

    def __init__(self):
        library = find_sdl3()
        if library is None:
            raise OSError("libSDL3.so.0 was not found")
        pointer, void, boolean = ctypes.c_void_p, None, ctypes.c_bool
        for name, result, arguments in (
                ("SDL_Init", boolean, [ctypes.c_uint32]), ("SDL_GetError", ctypes.c_char_p, []),
                ("SDL_SetHint", boolean, [ctypes.c_char_p, ctypes.c_char_p]),
                ("SDL_PumpEvents", void, []), ("SDL_UpdateGamepads", void, []), ("SDL_Quit", void, []),
                ("SDL_GetGamepads", ctypes.POINTER(ctypes.c_uint32), [ctypes.POINTER(ctypes.c_int)]),
                ("SDL_free", void, [pointer]), ("SDL_OpenGamepad", pointer, [ctypes.c_uint32]),
                ("SDL_CloseGamepad", void, [pointer]), ("SDL_GetGamepadName", ctypes.c_char_p, [pointer]),
                ("SDL_GetGamepadButton", boolean, [pointer, ctypes.c_int]),
                ("SDL_GetGamepadAxis", ctypes.c_int16, [pointer, ctypes.c_int]),
                ("SDL_GetGamepadButtonLabel", ctypes.c_int, [pointer, ctypes.c_int])):
            function = getattr(library, name)
            function.restype, function.argtypes = result, arguments
        self.sdl = library
        # SDL would catch SIGINT and SIGTERM and turn them into events nobody reads: Ctrl-C would stop working.
        library.SDL_SetHint(b"SDL_NO_SIGNAL_HANDLERS", b"1")
        if not library.SDL_Init(INIT_GAMEPAD):
            raise OSError((library.SDL_GetError() or b"SDL_Init failed").decode(errors="replace"))
        self.open = {}  # joystick id -> SDL_Gamepad

    def close(self):
        """Shut SDL3 down. Without this the process aborts (SIGABRT) when it ends: SDL's input threads outlive the interpreter."""
        sdl, self.sdl = self.sdl, None
        if sdl is None: return
        for handle in self.open.values(): sdl.SDL_CloseGamepad(handle)
        self.open.clear()
        sdl.SDL_Quit()

    def read(self):
        sdl = self.sdl
        if sdl is None: return []
        sdl.SDL_PumpEvents(); sdl.SDL_UpdateGamepads()
        count = ctypes.c_int(0)
        listed = sdl.SDL_GetGamepads(ctypes.byref(count))
        ids = [listed[index] for index in range(count.value)] if listed else []
        if listed: sdl.SDL_free(listed)
        for gone in set(self.open) - set(ids):
            sdl.SDL_CloseGamepad(self.open.pop(gone))
        pads = []
        for identity in ids:
            if identity not in self.open:
                handle = sdl.SDL_OpenGamepad(identity)
                if not handle: continue
                self.open[identity] = handle
            handle = self.open[identity]
            pads.append({"name": (sdl.SDL_GetGamepadName(handle) or b"").decode(errors="replace"),
                         "buttons": {b for b in range(0, 15) if sdl.SDL_GetGamepadButton(handle, b)},
                         "axes": {axis: sdl.SDL_GetGamepadAxis(handle, axis) / 32767.0 for axis in (AXIS_LEFTX, AXIS_LEFTY)},
                         "labels": {b: sdl.SDL_GetGamepadButtonLabel(handle, b) for b in (SOUTH, EAST, WEST, NORTH)}})
        return pads


def in_game_mode(environment=None):
    """True when the launcher runs where a pad is the only input: Steam's Game Mode or a gamescope session.
    SteamGamepadUI is what one forum report found Steam setting in Game Mode, and GAMESCOPE_WAYLAND_DISPLAY is the
    variable gamescope exports to its clients; neither is documented by Valve, so the first pad press also
    switches the layout."""
    environment = os.environ if environment is None else environment
    return environment.get("SteamGamepadUI") == "1" or bool(environment.get("GAMESCOPE_WAYLAND_DISPLAY"))


STATUS = {"reason": ""}  # why open_backend() returned None
TRACE = os.environ.get("BIFROST_GAMEPAD_DEBUG") == "1"


def trace(*parts):
    """With BIFROST_GAMEPAD_DEBUG=1, say on stderr what the library does with the pad, step by step."""
    if TRACE:
        print("gamepad:", *parts, file=sys.stderr, flush=True)


def open_backend():
    """An `Sdl3Backend`, or None when SDL3 is missing or has no gamepad support: the launcher then works as before.
    STATUS["reason"] says why not."""
    if os.environ.get("BIFROST_NO_GAMEPAD") == "1":
        STATUS["reason"] = "BIFROST_NO_GAMEPAD=1"
        return None
    try:
        backend = Sdl3Backend()
    except (OSError, AttributeError) as error:
        STATUS["reason"] = str(error) or type(error).__name__
        return None
    STATUS["reason"] = ""
    return backend


ACTIVE_INTERVAL, IDLE_INTERVAL, SEARCH_INTERVAL = 16, 300, 500  # ms: pad connected and window in front, not in front, no pad


def connect(window, application, backend):
    """Poll `backend` for the library `window` and press keys for it. Returns the PadReader.

    `window.pad_action(action)` runs first and returns True for what only the library does (switching layout, sorting,
    its menu, closing the details drawer); the rest becomes key presses on the widget that has the focus."""
    from PyQt6.QtCore import QEvent, QTimer, Qt
    from PyQt6.QtGui import QKeyEvent
    from PyQt6.QtWidgets import QAbstractButton, QAbstractItemView, QComboBox, QMenu

    key = Qt.Key
    names = {"Up": (key.Key_Up, Qt.KeyboardModifier.NoModifier), "Down": (key.Key_Down, Qt.KeyboardModifier.NoModifier),
             "Left": (key.Key_Left, Qt.KeyboardModifier.NoModifier), "Right": (key.Key_Right, Qt.KeyboardModifier.NoModifier),
             "Tab": (key.Key_Tab, Qt.KeyboardModifier.NoModifier), "Backtab": (key.Key_Backtab, Qt.KeyboardModifier.ShiftModifier),
             "Space": (key.Key_Space, Qt.KeyboardModifier.NoModifier), "Return": (key.Key_Return, Qt.KeyboardModifier.NoModifier),
             "Escape": (key.Key_Escape, Qt.KeyboardModifier.NoModifier), "Menu": (key.Key_Menu, Qt.KeyboardModifier.NoModifier),
             "Ctrl+Tab": (key.Key_Tab, Qt.KeyboardModifier.ControlModifier),
             "Ctrl+Backtab": (key.Key_Backtab, Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier)}
    reader, timer, shown = PadReader(backend), QTimer(window), [None]   # shown: the pad name the window was last told of
    front = [None]                                                       # whether a window was in front at the last tick

    def kind(widget, action):
        if widget is not None and widget.objectName() == "card": return "card"
        if widget is not None and widget.objectName() == "padrow": return "press"      # a row of the gamepad settings page
        if isinstance(widget, QMenu): return "menu"
        if isinstance(widget, QAbstractItemView):
            # A table in a dialog keeps Up and Down until its cursor is at the end, then they leave it.
            model, row = widget.model(), widget.currentIndex().row()
            if (action == "up" and row <= 0) or (action == "down" and model is not None and row >= model.rowCount() - 1):
                return "edge"
            return "menu"
        if isinstance(widget, (QAbstractButton, QComboBox)): return "press"
        return "other"

    def press(name, widget):
        code, modifiers = names[name]
        for event in (QEvent.Type.KeyPress, QEvent.Type.KeyRelease):
            application.sendEvent(widget, QKeyEvent(event, code, modifiers))

    def handle(action):
        if window.pad_action(action):
            trace(action, "-> taken by the library window"); return
        popup = application.activePopupWidget()
        # A dialog, or a page or sheet drawn inside the library window, may give Start and Y their own meaning (the gamepad
        # settings page: save, restore defaults).
        modal = application.activeModalWidget() or getattr(window, "sheet_widget", None)
        hook = {"options": "pad_options", "menu": "pad_menu"}.get(action)
        if modal is not None and popup is None and hook and hasattr(modal, hook):
            trace(action, "-> taken by the dialog (" + type(modal).__name__ + ")")
            getattr(modal, hook)(); return
        widget = (popup.focusWidget() or popup) if popup else (application.focusWidget() or application.activeWindow())
        if widget is None:
            trace(action, "-> nothing has the focus"); return
        # Whatever is in a popup (a combo box's list, a menu) takes the arrows and Return itself.
        where = "menu" if popup else kind(widget, action)
        if where == "edge" and action in ("up", "down"):
            # Not a Tab key, and not the table's own focusNextChild: a table steps between cells with both. The window's
            # focus chain is what gets out.
            window_ = application.activeWindow() or widget
            (window_.focusPreviousChild if action == "up" else window_.focusNextChild)(); return
        inside = application.activeModalWidget() is not None or (hasattr(window, "sheet_active") and window.sheet_active())
        keys = keys_for(action, where, inside)
        trace(action, "-> keys", keys or "none", "to", type(widget).__name__, "(" + where + ")")
        for name in keys: press(name, widget)

    def in_front():
        """Whether one of this application's windows has the user's attention: a popup menu or a dialog that has just
        opened counts, since Qt can report no active window while one of them has the focus."""
        return (application.activeWindow() is not None or application.activePopupWidget() is not None
                or application.activeModalWidget() is not None)

    def tick():
        # PyQt aborts the whole process on an exception in a slot: a failing pad must not take the library with it.
        try:
            if not in_front():
                if front[0] is not False: trace("no window of this application is in front: ignoring the pad")
                front[0] = False
                reader.reset(); timer.setInterval(IDLE_INTERVAL); return
            if front[0] is not True: trace("a window is in front: reading the pad")
            front[0] = True
            for action in reader.poll(): handle(action)
            if reader.name != shown[0]:
                trace("controller:", reader.name or "none")
                shown[0] = reader.name
                if hasattr(window, "pad_status"): window.pad_status(reader.name)
            timer.setInterval(ACTIVE_INTERVAL if reader.connected else SEARCH_INTERVAL)
        except Exception:
            traceback.print_exc()
            print("Asgard: gamepad control is off for this session.", file=sys.stderr)
            timer.stop()

    timer.timeout.connect(tick); timer.start(SEARCH_INTERVAL)
    close = getattr(backend, "close", None)
    if close: application.aboutToQuit.connect(close)
    window.pad_reader, window.pad_tick, window.pad_timer = reader, tick, timer
    return reader


def focus_ring(dialog):
    """Draw a ring around the widget that has the focus inside `dialog`. Check boxes and combo boxes show no focus of their
    own under the library's stylesheet, and someone steering with a pad cannot tell where they are."""
    from PyQt6.QtCore import QEvent, QObject, QPoint, QRect, QTimer, Qt
    from PyQt6.QtWidgets import QApplication, QFrame

    application = QApplication.instance()
    ring = QFrame(dialog); ring.setObjectName("focusring")
    ring.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
    ring.setStyleSheet("QFrame#focusring { border: 3px solid palette(highlight); border-radius: 9px; background: transparent; }")
    ring.hide()

    def release(*_):
        """Stop following the focus. Safe to repeat: QDialog emits `finished` on every accept() or reject()."""
        try: application.focusChanged.disconnect(place)
        except (TypeError, RuntimeError): pass

    def place(*_):
        try:
            widget = application.focusWidget()
            if widget is None or widget is ring or not dialog.isAncestorOf(widget) or not widget.isVisible():
                ring.hide(); return
            corner = widget.mapTo(dialog, QPoint(0, 0))
            ring.setGeometry(QRect(corner, widget.size()).adjusted(-4, -3, 4, 3)); ring.show(); ring.raise_()
        except RuntimeError:
            release()    # the dialog was deleted without finishing: its slot must not outlive it (PyQt aborts on a slot's exception)

    class Follow(QObject):
        def eventFilter(self, watched, event):
            if event.type() in (QEvent.Type.Resize, QEvent.Type.LayoutRequest, QEvent.Type.Show):
                QTimer.singleShot(0, place)
            return False

    follow = Follow(dialog); dialog.installEventFilter(follow)
    application.focusChanged.connect(place)
    dialog.finished.connect(release)
    return ring


def main():
    """`python3 android_gamepad.py`: what the launcher's gamepad control sees, live. Press buttons and move the left stick;
    each line is a button or stick change as SDL3 reports it, and the launcher action it makes. Ctrl-C stops."""
    sys.stdout.reconfigure(line_buffering=True)
    backend = open_backend()
    if backend is None:
        print("Gamepad control is off: " + STATUS["reason"])
        return 1
    reader, last, announced = PadReader(backend), None, None
    print("SDL3 is loaded. Looking for a controller; press buttons and move the left stick. Ctrl-C stops.")
    try:
        while True:
            pads = backend.read()
            names = [pad["name"] for pad in pads]
            if names != announced:
                announced = names
                print("Controllers: " + (", ".join(names) if names else "none found yet"))
            state = [(pad["name"], sorted(pad["buttons"]), {axis: round(value, 2) for axis, value in pad["axes"].items() if abs(value) > 0.2})
                     for pad in pads]
            if pads and state != last:
                print("  held:", [(name, buttons, axes) for name, buttons, axes in state if buttons or axes] or "nothing")
            last = state
            actions = reader.poll()
            if actions: print("  -> launcher action: " + ", ".join(actions))
            time.sleep(0.016)
    except KeyboardInterrupt:
        print()
        return 0
    finally:
        backend.close()


if __name__ == "__main__":
    sys.exit(main())
