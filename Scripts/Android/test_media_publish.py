#!/usr/bin/env python3
"""A picture an app saves through MediaStore is copied to ~/Pictures/Bifrost/<app>, and nothing else is.

`FrameworkMediaStore` asks the supervisor (`publish_media`) to copy a finished image out of the app's shared storage.
The app is untrusted, so the copy must read only a regular file below /storage/emulated/0/DCIM or /Pictures of its
root, never through a symlink at any step (the app can replace a folder of the path with a link to a host file), and
must never replace a file already in the host folder. These tests drive `publish_media` and the Pictures-folder lookup
on temporary directories; the guest side is `ManagedServiceBatch`'s MANAGED_MEDIA_STORE_OK.
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import android_processes

PNG = b"\x89PNG\r\n\x1a\nbifrost"


class PublishMediaTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        base = Path(self.temporary.name)
        self.root, self.destination = base / "root", base / "Pictures/Bifrost/Umamusume"
        self.pictures = self.root / "storage/emulated/0/Pictures"
        self.pictures.mkdir(parents=True)

    def tearDown(self):
        self.temporary.cleanup()

    def publish(self, path):
        return android_processes.publish_media(self.root, path, self.destination)

    def test_a_saved_picture_is_copied_with_its_bytes(self):
        (self.pictures / "photo.png").write_bytes(PNG)
        self.assertEqual(self.publish("/storage/emulated/0/Pictures/photo.png"), {"name": "photo.png"})
        self.assertEqual((self.destination / "photo.png").read_bytes(), PNG)
        self.assertEqual(sorted(p.name for p in self.destination.iterdir()), ["photo.png"], "a temporary file was left")

    def test_a_second_picture_of_the_same_name_never_replaces_the_first(self):
        self.destination.mkdir(parents=True)
        (self.destination / "photo.png").write_bytes(b"mine")
        (self.pictures / "photo.png").write_bytes(PNG)
        self.assertEqual(self.publish("/storage/emulated/0/Pictures/photo.png"), {"name": "photo (1).png"})
        self.assertEqual(self.publish("/storage/emulated/0/Pictures/photo.png"), {"name": "photo (2).png"})
        self.assertEqual((self.destination / "photo.png").read_bytes(), b"mine")
        self.assertEqual((self.destination / "photo (2).png").read_bytes(), PNG)

    def test_dcim_and_subfolders_are_published(self):
        (self.root / "storage/emulated/0/DCIM/Camera").mkdir(parents=True)
        (self.root / "storage/emulated/0/DCIM/Camera/shot.jpg").write_bytes(PNG)
        (self.pictures / "Game").mkdir()
        (self.pictures / "Game/card").write_bytes(PNG)
        self.assertEqual(self.publish("/storage/emulated/0/DCIM/Camera/shot.jpg"), {"name": "shot.jpg"})
        self.assertEqual(self.publish("/storage/emulated/0/Pictures/Game/card"), {"name": "card"})

    def test_paths_outside_the_picture_folders_are_refused(self):
        (self.root / "storage/emulated/0/Download").mkdir(parents=True)
        (self.root / "storage/emulated/0/Download/x.png").write_bytes(PNG)
        (self.pictures / ".hidden.png").write_bytes(PNG)
        for path in ("/storage/emulated/0/Download/x.png", "/storage/emulated/0/Pictures/../Download/x.png",
                     "/storage/emulated/0/Pictures//photo.png", "storage/emulated/0/Pictures/photo.png",
                     "/data/user/0/app/Pictures/photo.png", "/storage/emulated/0/Pictures",
                     "/storage/emulated/0/Pictures/.hidden.png", "/storage/emulated/0/Pictures/a\x00b.png", 7):
            with self.subTest(path=path), self.assertRaises(ValueError):
                self.publish(path)
        self.assertFalse(self.destination.exists() and any(self.destination.iterdir()))

    def test_a_symlink_at_any_step_is_refused(self):
        secret = Path(self.temporary.name) / "secret"
        secret.mkdir()
        (secret / "key.png").write_bytes(b"host file")
        (self.pictures / "link.png").symlink_to(secret / "key.png")
        (self.pictures / "folder").symlink_to(secret)
        for path in ("/storage/emulated/0/Pictures/link.png", "/storage/emulated/0/Pictures/folder/key.png"):
            with self.subTest(path=path), self.assertRaises(OSError):
                self.publish(path)
        # A link replacing a folder higher up, the shared-storage root itself.
        storage = self.root / "storage/emulated/0"
        os.rename(storage, self.root / "storage/emulated/moved")
        storage.symlink_to(self.root / "storage/emulated/moved")
        with self.assertRaises(OSError):
            self.publish("/storage/emulated/0/Pictures/link.png")
        self.assertFalse(self.destination.exists() and any(self.destination.iterdir()))

    def test_special_and_oversized_files_are_refused(self):
        os.mkfifo(self.pictures / "pipe.png")
        with self.assertRaises(ValueError):
            self.publish("/storage/emulated/0/Pictures/pipe.png")
        (self.pictures / "big.png").write_bytes(b"x" * 64)
        with mock.patch.object(android_processes, "MAX_MEDIA_FILE", 32), self.assertRaises(ValueError):
            self.publish("/storage/emulated/0/Pictures/big.png")


class PicturesDirectoryTests(unittest.TestCase):
    def directory(self, text=None):
        with tempfile.TemporaryDirectory() as home:
            config = Path(home) / "config"
            config.mkdir()
            if text is not None:
                (config / "user-dirs.dirs").write_text(text)
            result = android_processes.pictures_directory({"HOME": home, "XDG_CONFIG_HOME": str(config)})
            return str(result).replace(home, "~")

    def test_the_desktop_setting_is_followed(self):
        self.assertEqual(self.directory('# comment\nXDG_PICTURES_DIR="$HOME/Bilder"\n'), "~/Bilder")
        self.assertEqual(self.directory('XDG_PICTURES_DIR="/mnt/photos"\n'), "/mnt/photos")

    def test_no_setting_or_a_disabled_one_means_pictures(self):
        self.assertEqual(self.directory(), "~/Pictures")
        self.assertEqual(self.directory('XDG_PICTURES_DIR="$HOME"\n'), "~/Pictures")
        self.assertEqual(self.directory('XDG_PICTURES_DIR="relative"\n'), "~/Pictures")

    def test_the_folder_is_the_app_label_as_one_safe_name(self):
        self.assertEqual(android_processes.media_folder("Umamusume", "com.cygames.umamusume"), "Umamusume")
        self.assertEqual(android_processes.media_folder("A/B\\C\nD", "p"), "A B C D")
        self.assertEqual(android_processes.media_folder("..", "com.example"), "com.example")
        self.assertEqual(android_processes.media_folder(None, "com.example"), "com.example")


if __name__ == "__main__":
    unittest.main()
