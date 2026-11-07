#!/usr/bin/env python3
"""install-deps.sh: detection, installing only what is missing, sudo only when needed, a dry run, tools and
libraries found without their package; find-host-tools.sh: tools off PATH; fetch-sdl3.py: refusing an SDL3 build
that has no window system.

Offline and changes nothing: fake package managers and sudo earlier on PATH log what they are asked."""
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "Scripts/Setup/install-deps.sh"
LOGGER = '#!/bin/bash\nprintf "%s %s\\n" "$(basename "$0")" "$*" >> "$FAKE_LOG"\n'
# Each query tool succeeds for a package named in $FAKE_INSTALLED.
QUERIES = {
    "dpkg-query": 'for last; do :; done\ncase " $FAKE_INSTALLED " in *" $last "*) printf installed; exit 0 ;; esac\nexit 1\n',
    "rpm": 'for last; do :; done\ncase " $FAKE_INSTALLED " in *" $last "*) exit 0 ;; esac\nexit 1\n',
    "pacman-query": 'case "$1" in -T) case " $FAKE_INSTALLED " in *" $2 "*) exit 0 ;; esac; exit 127 ;; esac\n',
}


class InstallDepsTests(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="bifrost-install-deps."))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        self.log = self.work / "log"
        self.tool("sudo", LOGGER + 'exec "$@"\n')
        for tool in ("id", "sed", "basename", "bash"):
            os.symlink(shutil.which(tool), self.bin / tool)

    def tool(self, name, text):
        (self.bin / name).write_text(text if text.startswith("#!") else "#!/bin/bash\n" + text)
        (self.bin / name).chmod(0o755)

    def manager(self, name):
        if name == "apt":
            self.tool("apt-get", LOGGER)
            self.tool("apt-cache", 'case "$2" in libsdl3-dev) [ -n "$FAKE_HAS_SDL3" ] ;; *) exit 0 ;; esac\n')
            self.tool("dpkg-query", QUERIES["dpkg-query"])
        elif name == "pacman":
            self.tool("pacman", LOGGER + QUERIES["pacman-query"])
        else:
            self.tool(name, LOGGER)
            self.tool("rpm", QUERIES["rpm"])

    def run_script(self, *arguments, installed="", has_sdl3="", expect=0, **extra_env):
        result = subprocess.run([str(self.bin / "bash"), str(SCRIPT), *arguments], capture_output=True, text=True,
                                env={"PATH": str(self.bin), "FAKE_LOG": str(self.log), "FAKE_INSTALLED": installed,
                                     "FAKE_HAS_SDL3": has_sdl3, "BIFROST_TOOL_SEARCH": "0", **extra_env})
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def logged(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def installs(self):
        return [line for line in self.logged() if not line.startswith("sudo ") and " -T" not in line]

    @unittest.skipIf(os.geteuid() == 0, "root does not use sudo")
    def test_each_manager_installs_with_its_own_command_and_names(self):
        for name, command, sample in (("dnf", "dnf install", "SDL3-devel"), ("zypper", "zypper install", "pkgconfig(libavcodec)"),
                                      ("pacman", "pacman -S --needed", "python-pyqt6")):
            with self.subTest(manager=name):
                self.manager(name)
                self.run_script()
                install = self.installs()[-1]
                self.assertTrue(install.startswith(command + " "), install)
                self.assertIn(sample, install.split())
                self.assertTrue(any(line.startswith(f"sudo {command}") for line in self.logged()))
                self.log.unlink()
                (self.bin / name).unlink()

    def test_only_what_is_missing_is_installed(self):
        self.manager("pacman")
        self.run_script("--no-asgard", installed="base-devel pkgconf git python curl xz diffutils ffmpeg sdl3 "
                                                 "sdl2-compat mesa libglvnd libffi wayland")
        self.assertEqual(self.installs(), ["pacman -S --needed libx11"])

    def test_nothing_missing_needs_no_sudo(self):
        self.manager("pacman")
        output = self.run_script("--no-asgard", installed="base-devel pkgconf git python curl xz diffutils ffmpeg sdl3 "
                                                          "sdl2-compat mesa libglvnd libffi wayland libx11")
        self.assertIn("are installed", output)
        self.assertEqual(self.installs(), [])
        self.assertFalse(any(line.startswith("sudo ") for line in self.logged()))

    def test_any_existing_jdk_counts(self):
        self.manager("pacman")
        self.tool("javac", "exit 0\n")
        self.tool("keytool", "exit 0\n")
        self.run_script()
        self.assertNotIn("jdk-openjdk", self.installs()[-1].split())

    def test_no_asgard_leaves_out_the_asgard_tools(self):
        self.manager("pacman")
        self.run_script("--no-asgard")
        self.assertNotIn("clang", self.installs()[-1].split())
        self.run_script()
        for package in ("clang", "lld", "jdk-openjdk", "e2fsprogs", "binutils", "python-pyqt6"):
            self.assertIn(package, self.installs()[-1].split())

    def test_dry_run_installs_nothing(self):
        for name in ("apt", "dnf", "zypper", "pacman"):
            with self.subTest(manager=name):
                self.manager(name)
                output = self.run_script("--dry-run", "--manager", name)
                self.assertIn("Would run:", output)
                self.assertEqual(self.installs(), [])

    def test_apt_refreshes_first_and_a_missing_sdl3_is_a_warning(self):
        self.manager("apt")
        output = self.run_script()
        self.assertEqual(self.installs()[0], "apt-get update")
        self.assertNotIn("libsdl3-dev", self.installs()[1].split())
        self.assertIn("libsdl2-dev", self.installs()[1].split())
        self.assertIn("warning: this distribution has no libsdl3-dev", output)

    def test_apt_installs_sdl3_where_it_exists(self):
        self.manager("apt")
        self.run_script(has_sdl3="yes")
        self.assertIn("libsdl3-dev", self.installs()[1].split())

    def test_debian_with_the_pacman_game_still_uses_apt(self):
        self.manager("apt")
        self.tool("pacman", "exit 0\n")
        self.assertIn("apt-get install", self.run_script("--dry-run", has_sdl3="yes"))

    def test_a_tool_off_path_counts_as_installed(self):
        """debugfs lives in /usr/sbin on Debian, where an ordinary user's PATH does not look."""
        self.manager("pacman")
        sbin = self.work / "sbin"
        sbin.mkdir()
        for tool in ("debugfs", "readelf"):
            (sbin / tool).write_text("#!/bin/sh\n")
            (sbin / tool).chmod(0o755)
        output = self.run_script(BIFROST_TOOL_DIRS=str(sbin))
        self.assertNotIn("e2fsprogs", self.installs()[-1].split())
        self.assertNotIn("binutils", self.installs()[-1].split())
        self.assertIn("e2fsprogs: not installed by pacman, but found (", output)
        self.assertIn("clang", self.installs()[-1].split())  # what is not found is still installed

    def test_a_library_pkg_config_finds_counts_as_installed(self):
        self.manager("pacman")
        self.tool("pkg-config", 'case "$*" in *sdl3*|*x11*) exit 0 ;; esac\nexit 1\n')
        self.run_script("--no-asgard", installed="base-devel pkgconf git python curl xz diffutils ffmpeg sdl2-compat "
                                                 "mesa libglvnd libffi wayland")
        self.assertEqual(self.installs(), [])  # sdl3 and libx11 were found by pkg-config, not pacman

    def test_a_jdk_off_path_counts_as_installed(self):
        self.manager("pacman")
        jdk = self.work / "jvm/java-25-openjdk/bin"
        jdk.mkdir(parents=True)
        for tool in ("javac", "keytool"):
            (jdk / tool).write_text("#!/bin/sh\n")
            (jdk / tool).chmod(0o755)
        self.run_script(BIFROST_TOOL_DIRS=str(jdk))
        self.assertNotIn("jdk-openjdk", self.installs()[-1].split())

    def test_without_an_sdl3_package_apt_installs_what_building_it_needs(self):
        self.manager("apt")
        output = self.run_script()
        installed = self.installs()[1].split()
        for package in ("cmake", "libxkbcommon-dev", "wayland-protocols", "libwayland-bin", "libxext-dev"):
            self.assertIn(package, installed)
        self.assertIn("SDL3 will be built from source", output)
        self.log.unlink()
        self.run_script("--no-asgard")  # the Android window is Asgard's: nothing to build SDL3 for
        self.assertNotIn("cmake", self.installs()[1].split())

    def test_an_sdl3_already_built_in_the_checkout_needs_nothing(self):
        self.manager("apt")
        self.tool("pkg-config", 'case "$*" in *"--atleast-version 3.2.0 sdl3"*) exit 0 ;; esac\nexit 1\n')
        output = self.run_script()
        self.assertNotIn("SDL3 will be built", output)
        self.assertNotIn("cmake", self.installs()[1].split())

    def test_an_unsupported_system_lists_what_to_install(self):
        output = self.run_script(expect=1)
        self.assertIn("no supported package manager", output)
        self.assertIn("PyQt6", output)

    def test_a_failed_install_is_an_error(self):
        self.manager("pacman")
        self.tool("pacman", QUERIES["pacman-query"] + "exit 1\n")
        output = self.run_script(expect=1)
        self.assertIn("could not install", output)
        self.assertIn("pacman -Syu", output)

    def test_an_unknown_manager_is_refused(self):
        self.assertIn("unknown package manager", self.run_script("--manager", "brew", expect=2))


FIND = Path(__file__).resolve().parents[2] / "Scripts/Setup/find-host-tools.sh"


class FindHostToolsTests(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="bifrost-find-tools."))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)
        self.sbin, self.links = self.work / "sbin", self.work / "host-bin"
        self.sbin.mkdir()
        path = self.work / "path"  # only what the script needs, so the host's own tools cannot be found
        path.mkdir()
        for name in ("bash", "sh", "ln", "mkdir", "rm", "ls", "sort", "head"):
            os.symlink(shutil.which(name), path / name)
        self.env = {"PATH": str(path), "BIFROST_TOOL_SEARCH": "0", "BIFROST_TOOL_DIRS": str(self.sbin),
                    "BIFROST_HOST_BIN": str(self.links), "HOME": str(self.work)}

    def tool(self, directory, name):
        (directory / name).parent.mkdir(parents=True, exist_ok=True)
        (directory / name).write_text("#!/bin/sh\n")
        (directory / name).chmod(0o755)

    def run_find(self, *arguments, expect=0, **env):
        result = subprocess.run(["bash", str(FIND), *arguments], capture_output=True, text=True, env={**self.env, **env})
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_a_tool_off_path_is_linked_for_the_setup_scripts(self):
        self.tool(self.sbin, "debugfs")
        self.assertIn("Found off PATH, using: debugfs", self.run_find("debugfs", "sh"))
        self.assertEqual((self.links / "debugfs").resolve(), (self.sbin / "debugfs").resolve())
        self.assertFalse((self.links / "sh").exists())  # a tool on PATH is never shadowed or linked

    def test_a_versioned_tool_is_found_by_its_plain_name_newest_first(self):
        self.tool(self.sbin, "clang-17")
        self.tool(self.sbin, "clang-18")
        self.run_find("clang")
        self.assertEqual((self.links / "clang").resolve().name, "clang-18")

    def test_a_missing_tool_is_named_with_what_provides_it_and_what_to_do(self):
        self.tool(self.sbin, "readelf")
        output = self.run_find("readelf", "debugfs", "ld.lld", expect=1)
        self.assertIn("could not find: debugfs ld.lld", output)
        self.assertIn("debugfs: provided by e2fsprogs", output)
        self.assertIn("ld.lld: provided by lld", output)
        self.assertIn("BIFROST_TOOL_DIRS", output)
        self.assertNotIn("readelf:", output)

    def test_where_prints_the_path(self):
        self.tool(self.sbin, "debugfs")
        self.assertEqual(self.run_find("--where", "debugfs").strip(), str(self.sbin / "debugfs"))
        self.run_find("--where", "no-such-tool", expect=1)


class FetchSdl3Tests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("fetch_sdl3", Path(__file__).resolve().parents[2] / "Tools/Fetch/fetch-sdl3.py")
        self.fetch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.fetch)
        self.work = Path(tempfile.mkdtemp(prefix="bifrost-sdl3."))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)

    def config(self, *defines):
        header = self.work / "include-config-release/build_config/SDL_build_config.h"
        header.parent.mkdir(parents=True)
        header.write_text("".join(f"#define SDL_VIDEO_DRIVER_{name} 1\n" for name in defines) + "/* #undef SDL_VIDEO_DRIVER_WAYLAND */\n")

    def test_a_build_without_x11_or_wayland_is_recognised(self):
        self.config()
        self.assertEqual(self.fetch.video_drivers(self.work), set())

    def test_the_window_systems_a_build_has_are_listed(self):
        self.config("X11", "DUMMY")
        self.assertEqual(self.fetch.video_drivers(self.work), {"X11"})


if __name__ == "__main__":
    unittest.main()
