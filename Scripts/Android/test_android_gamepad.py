#!/usr/bin/env python3
"""android_gamepad.py: pad states to launcher actions (edges, repeat, stick, hold-back), the key a pad action presses,
and the SDL3 backend's absence."""
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import android_gamepad as pads


class Backend:
    def __init__(self):
        self.pads = []

    def pad(self, buttons=(), x=0.0, y=0.0, labels=None):
        self.pads = [{"name": "Test pad", "buttons": set(buttons), "axes": {pads.AXIS_LEFTX: x, pads.AXIS_LEFTY: y},
                      "labels": labels or {pads.SOUTH: 1, pads.EAST: 2, pads.WEST: 3, pads.NORTH: 4}}]

    def read(self):
        return self.pads


class Clock:
    now = 100.0

    def __call__(self):
        return self.now


class PadReaderTests(unittest.TestCase):
    def setUp(self):
        self.backend, self.clock = Backend(), Clock()
        self.reader = pads.PadReader(self.backend, self.clock)
        self.backend.pad()
        self.assertEqual(self.reader.poll(), [])   # the first poll only records what is held

    def test_a_button_fires_once_when_pressed(self):
        self.backend.pad({pads.SOUTH})
        self.assertEqual(self.reader.poll(), ["accept"])
        self.assertEqual(self.reader.poll(), [])
        self.backend.pad()
        self.assertEqual(self.reader.poll(), [])
        self.backend.pad({pads.EAST, pads.NORTH})
        self.assertEqual(self.reader.poll(), ["back", "menu"])

    def test_buttons_map_to_the_launcher_actions(self):
        for button, action in ((pads.WEST, "sort"), (pads.START, "options"), (pads.LEFT_SHOULDER, "page_up"),
                               (pads.RIGHT_SHOULDER, "page_down"), (pads.DPAD_UP, "up"), (pads.DPAD_RIGHT, "right")):
            self.backend.pad({button})
            self.assertEqual(self.reader.poll(), [action])
            self.backend.pad(); self.reader.poll()
        self.backend.pad({pads.GUIDE})
        self.assertEqual(self.reader.poll(), [])   # Steam's own button is not ours
        self.backend.pad(); self.reader.poll()
        self.backend.pad({pads.BACK})
        self.assertEqual(self.reader.poll(), ["details"])   # Select opens the last session's details

    def test_a_held_direction_repeats_after_a_delay_and_a_held_button_does_not(self):
        self.backend.pad({pads.DPAD_DOWN, pads.SOUTH})
        self.assertEqual(self.reader.poll(), ["accept", "down"])
        self.clock.now += pads.REPEAT_DELAY - 0.01
        self.assertEqual(self.reader.poll(), [])
        self.clock.now += 0.02
        self.assertEqual(self.reader.poll(), ["down"])
        self.clock.now += pads.REPEAT_INTERVAL + 0.001
        self.assertEqual(self.reader.poll(), ["down"])
        self.backend.pad()
        self.clock.now += 5
        self.assertEqual(self.reader.poll(), [])

    def test_the_left_stick_is_a_dpad_with_hysteresis(self):
        self.backend.pad(x=0.3)
        self.assertEqual(self.reader.poll(), [])                # inside the dead zone
        self.backend.pad(x=0.6, y=0.2)
        self.assertEqual(self.reader.poll(), ["right"])         # the stronger axis wins
        self.backend.pad(x=0.4, y=0.2)
        self.assertEqual(self.reader.poll(), [])                # still leaning: held, not pressed again
        self.assertEqual(self.reader.stick, "right")
        self.backend.pad(x=0.2)
        self.reader.poll()
        self.assertIsNone(self.reader.stick)
        self.backend.pad(x=0.1, y=-0.9)
        self.assertEqual(self.reader.poll(), ["up"])

    def test_reset_ignores_what_is_already_down_until_it_is_released(self):
        self.reader.reset()
        self.backend.pad({pads.SOUTH, pads.DPAD_LEFT})
        self.assertEqual(self.reader.poll(), [])                # held while another window had the focus
        self.assertEqual(self.reader.poll(), [])
        self.backend.pad(); self.reader.poll()
        self.backend.pad({pads.SOUTH})
        self.assertEqual(self.reader.poll(), ["accept"])

    def test_any_pad_controls_and_the_buttons_take_the_pad_names(self):
        self.backend.pads = [{"name": "one", "buttons": set(), "axes": {}, "labels": {pads.SOUTH: 5, pads.EAST: 6}},
                             {"name": "two", "buttons": {pads.NORTH}, "axes": {}, "labels": {}}]
        self.assertEqual(self.reader.poll(), ["menu"])
        self.assertEqual(self.reader.label(pads.SOUTH, "A"), "Cross")        # a PlayStation pad
        self.assertEqual(self.reader.label(pads.EAST, "B"), "Circle")
        self.assertEqual(self.reader.label(pads.WEST, "X"), "X")              # unreported: the default
        self.assertTrue(self.reader.connected)
        self.assertEqual(self.reader.name, "one")
        self.backend.pads = []
        self.reader.poll()
        self.assertFalse(self.reader.connected)
        self.assertEqual(self.reader.name, "")


class KeysTests(unittest.TestCase):
    def test_the_card_grid_uses_plain_keys(self):
        for action, keys in (("up", ["Up"]), ("left", ["Left"]), ("accept", ["Return"]), ("back", ["Escape"]),
                             ("menu", ["Menu"])):
            self.assertEqual(pads.keys_for(action, "card", False), keys)
        self.assertEqual(pads.keys_for("page_up", "card", False), [])         # the library handles pages itself

    def test_a_dialog_walks_its_focus_order_with_up_and_down(self):
        self.assertEqual(pads.keys_for("down", "press", True), ["Tab"])
        self.assertEqual(pads.keys_for("up", "other", True), ["Backtab"])
        self.assertEqual(pads.keys_for("left", "press", True), ["Left"])      # sliders, tab bars and spin boxes keep them
        self.assertEqual(pads.keys_for("page_down", "press", True), ["Ctrl+Tab"])         # the next tab
        self.assertEqual(pads.keys_for("page_up", "press", True), ["Ctrl+Backtab"])
        self.assertEqual(pads.keys_for("down", "edge", True), ["Tab"])            # a table's last row: on to the next widget
        self.assertEqual(pads.keys_for("up", "edge", True), ["Backtab"])

    def test_menus_and_lists_keep_the_arrows_and_press_return(self):
        self.assertEqual(pads.keys_for("down", "menu", True), ["Down"])
        self.assertEqual(pads.keys_for("accept", "menu", True), ["Return"])

    def test_buttons_check_boxes_and_combo_boxes_are_pressed_with_space(self):
        self.assertEqual(pads.keys_for("accept", "press", True), ["Space"])
        self.assertEqual(pads.keys_for("menu", "press", True), [])
        self.assertEqual(pads.keys_for("sort", "card", False), [])


class BackendTests(unittest.TestCase):
    def test_without_sdl3_there_is_no_backend_and_no_error(self):
        with patch.object(pads, "find_sdl3", return_value=None):
            self.assertIsNone(pads.open_backend())

    def test_a_missing_sdl3_says_why(self):
        with patch.object(pads, "find_sdl3", return_value=None):
            pads.open_backend()
        self.assertIn("libSDL3", pads.STATUS["reason"])

    def test_the_opt_out_opens_nothing(self):
        with patch.dict(os.environ, {"BIFROST_NO_GAMEPAD": "1"}), patch.object(pads, "Sdl3Backend") as backend:
            self.assertIsNone(pads.open_backend())
            backend.assert_not_called()

    def test_game_mode_is_steams_or_gamescopes_variables(self):
        self.assertFalse(pads.in_game_mode({}))
        self.assertTrue(pads.in_game_mode({"SteamGamepadUI": "1"}))
        self.assertTrue(pads.in_game_mode({"GAMESCOPE_WAYLAND_DISPLAY": "gamescope-0"}))
        self.assertFalse(pads.in_game_mode({"SteamGamepadUI": "0", "SteamDeck": "1"}))   # desktop mode on a Deck


if __name__ == "__main__":
    unittest.main()
