"""Tests for rebrand.py. Run from the repository root: python3 -m unittest discover -s scripts/rebrand -v"""

import colorsys
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import rebrand  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]

try:
    from PIL import Image
except ImportError:
    Image = None


def renamed(text: str) -> str:
    return rebrand.rename_text(text.encode())[0].decode()


class RenameText(unittest.TestCase):
    def test_keeps_case_of_every_form(self):
        cases = {
            "Prism Launcher": "Crism Launcher",
            "PrismLauncher": "CrismLauncher",
            "prismlauncher": "crismlauncher",
            "PRISMLAUNCHER_JAVA_PATHS": "CRISMLAUNCHER_JAVA_PATHS",
            "m_prismVersionMajor": "m_crismVersionMajor",
            "PRISM_PRECOMPILED_BASE_HEADERS_H": "CRISM_PRECOMPILED_BASE_HEADERS_H",
            "org.prismlauncher.EntryPoint": "org.crismlauncher.EntryPoint",
            "PrismLaucher - Minecraft Launcher": "CrismLaucher - Minecraft Launcher",
            "updater/prismupdater/PrismUpdater.cpp": "updater/crismupdater/CrismUpdater.cpp",
        }
        for before, after in cases.items():
            with self.subTest(before=before):
                self.assertEqual(renamed(before), after)

    def test_counts_replacements(self):
        self.assertEqual(rebrand.rename_text(b"Prism prism PRISM")[1], 3)

    def test_protects_upstream_services(self):
        for text in [
            'set(Launcher_META_URL "https://meta.prismlauncher.org/v1/"',
            "https://i18n.prismlauncher.org/",
            "https://files.prismlauncher.org/fmllibs/",
            "https://prismlauncher.org/wiki/help-pages/%1",
            "https://prismlauncher.cachix.org",
            "https://hosted.weblate.org/projects/prismlauncher/launcher/",
            "https://opencollective.com/prismlauncher",
        ]:
            with self.subTest(text=text):
                self.assertNotIn("crism", renamed(text).lower())

    def test_renames_around_protected_spans(self):
        line = 'set(Launcher_WIKI_URL "https://prismlauncher.org/wiki/" CACHE STRING "Prism Launcher help")'
        self.assertEqual(renamed(line), line.replace("Prism Launcher help", "Crism Launcher help"))

    def test_protects_upstream_repository_links(self):
        line = "// See https://github.com/PrismLauncher/PrismLauncher/issues/6047. Prism handles it"
        self.assertEqual(renamed(line), line.replace("Prism handles", "Crism handles"))

    def test_protects_attribution_lines(self):
        for line in [
            " *  Copyright (C) 2022 Prism Launcher Contributors",
            "// SPDX-FileCopyrightText: 2023 Prism Launcher Contributors",
            'set(Launcher_Authors "MultiMC & Prism Launcher Contributors")',
        ]:
            with self.subTest(line=line):
                self.assertEqual(renamed(line), line)

    def test_renames_program_name_in_licence_header(self):
        self.assertEqual(renamed(" *  Prism Launcher - Minecraft Launcher"), " *  Crism Launcher - Minecraft Launcher")

    def test_protects_formats_and_handoffs(self):
        for line in [
            '{ "x-prismlauncher-loaders", loaders },',
            'table["x-prismlauncher-lock-update"].value_or(false)',
            'absoluteFilePath(".prism_launcher_updater_unpack.marker")',
            'url.scheme() == "prismlauncher" || url.scheme() == BuildConfig.LAUNCHER_APP_BINARY_NAME',
            "copied from: https://github.com/PrismarineJS/prismarine-auth/pull/44",
        ]:
            with self.subTest(line=line):
                self.assertEqual(renamed(line), line)

    def test_other_update_markers_are_renamed(self):
        self.assertEqual(renamed('"prism_launcher_update.log"'), '"crism_launcher_update.log"')
        self.assertEqual(renamed('".prism_launcher_update.lock"'), '".crism_launcher_update.lock"')

    def test_keep_markers_survive_and_disappear(self):
        data = b"Prism and " + rebrand.keep("Prism Launcher") + b" and Prism"
        self.assertEqual(rebrand.rename_text(data)[0], b"Crism and Prism Launcher and Crism")


class Leftovers(unittest.TestCase):
    def test_reports_unprotected_lines(self):
        data = b"ok\nhttps://meta.prismlauncher.org/\nPrism here\n"
        self.assertEqual(rebrand.leftovers(data), [3])

    def test_allowed_text_is_not_reported(self):
        data = b'tr("%1 Developers").arg("Prism Launcher")\n'
        self.assertEqual(rebrand.leftovers(data, [b'.arg("Prism Launcher")']), [])

    def test_clean_after_rename(self):
        source = (REPO_ROOT / "launcher/modplatform/packwiz/Packwiz.cpp").read_bytes()
        self.assertEqual(rebrand.leftovers(rebrand.rename_text(source)[0]), [])


class Recolor(unittest.TestCase):
    def hue(self, hex_digits: bytes) -> float:
        r, g, b = (int(hex_digits[i : i + 2], 16) / 255 for i in (0, 2, 4))
        return colorsys.rgb_to_hls(r, g, b)[0]

    def test_hues_land_in_band_in_order(self):
        # Prism's facets, in hue order: orange, yellow, green, teal, blue, purple, red
        facets = [b"fb9168", b"f3db6c", b"99cd61", b"7ab392", b"4b7cbc", b"6f488c", b"df6277"]
        band_offsets = [(self.hue(rebrand.recolor_hex(f)) - rebrand.HUE_BAND_START) % 1 for f in facets]
        for offset in band_offsets:
            self.assertLessEqual(offset, rebrand.HUE_BAND_WIDTH + 1e-6)
        self.assertEqual(band_offsets, sorted(band_offsets))

    def test_greys_unchanged(self):
        for grey in [b"ffffff", b"dfdfdf", b"777777", b"000000"]:
            with self.subTest(grey=grey):
                self.assertEqual(rebrand.recolor_hex(grey), grey)

    def test_short_hex(self):
        self.assertEqual(rebrand.recolor_hex(b"fff"), b"ffffff")

    def test_svg_only_touches_colours(self):
        svg = b'<path fill="#df6277" style="stroke:#4b7cbc;fill:url(#abc)"/><use href="#bad"/><stop stop-color="#fff"/>'
        out = rebrand.recolor_svg(svg)
        self.assertNotIn(b"#df6277", out)
        self.assertNotIn(b"#4b7cbc", out)
        self.assertIn(b"url(#abc)", out)
        self.assertIn(b'href="#bad"', out)
        self.assertIn(b'stop-color="#ffffff"', out)

    @unittest.skipIf(Image is None, "Pillow is not installed")
    def test_raster_formats_keep_sizes_and_alpha(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ["prismlauncher.ico", "prismlauncher.icns", "org.prismlauncher.PrismLauncher_256.png"]:
                with self.subTest(name=name):
                    src = REPO_ROOT / "program_info" / name
                    dst = Path(tmp, name)
                    dst.write_bytes(src.read_bytes())
                    rebrand.recolor_raster(dst)

                    with Image.open(src) as before, Image.open(dst) as after:
                        self.assertEqual(before.size, after.size)
                        if name.endswith(".ico"):
                            self.assertEqual(before.ico.sizes(), after.ico.sizes())
                        if name.endswith(".icns"):
                            self.assertEqual(sorted(before.info["sizes"]), sorted(after.info["sizes"]))
                        alpha = [im.convert("RGBA").getchannel("A").tobytes() for im in (before, after)]
                        self.assertEqual(alpha[0], alpha[1])
                        self.assertNotEqual(before.convert("RGB").tobytes(), after.convert("RGB").tobytes())

    @unittest.skipIf(Image is None, "Pillow is not installed")
    def test_recolor_image_matches_svg_hue(self):
        im = Image.new("RGBA", (1, 1), (0xDF, 0x62, 0x77, 255))
        r, g, b, _ = rebrand.recolor_image(im).getpixel((0, 0))
        expected = self.hue(rebrand.recolor_hex(b"df6277"))
        self.assertAlmostEqual(colorsys.rgb_to_hls(r / 255, g / 255, b / 255)[0], expected, delta=2 / 256)


class ReadmeLogo(unittest.TestCase):
    def test_recolored_and_renamed(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp, "nested", "logo.svg")
            rebrand.write_readme_logo(REPO_ROOT, out)
            svg = out.read_bytes()
        self.assertIn(b"<svg", svg)
        self.assertNotIn(b"#df6277", svg)
        self.assertIn(b"Crism Launcher Logo", svg)
        self.assertEqual(rebrand.leftovers(svg), [])


class Audit(unittest.TestCase):
    def test_ignores_untracked_files_such_as_submodules(self):
        # CI checks out cmake/vcpkg, whose third-party ports (ethindp-prism) aren't ours to rename
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "launcher").mkdir()
            (root / "launcher/a.cpp").write_bytes(b"Crism\n")
            port = root / "cmake/vcpkg/ports/ethindp-prism"
            port.mkdir(parents=True)
            (port / "vcpkg.json").write_bytes(b'"name": "ethindp-prism"\n')
            self.assertEqual(rebrand.audit(root, [], ["launcher/a.cpp"]), [])

    def test_checks_tracked_files_under_their_new_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "launcher").mkdir()
            (root / "launcher/crism.cpp").write_bytes(b"ok\nPrism left\n")
            problems = rebrand.audit(root, [], ["launcher/prism.cpp"])
        self.assertEqual(problems, ["launcher/crism.cpp:2: Prism left"])


class AgainstThisCheckout(unittest.TestCase):
    """Fails when upstream changes the text a targeted edit expects."""

    def test_targeted_edits_match(self):
        report = rebrand.Report()
        rebrand.apply_edits(REPO_ROOT, rebrand.targeted_edits("owner/Repo"), report, dry_run=True)
        self.assertEqual(report.errors, [])

    def test_logo_files_exist(self):
        logos = [f for f in rebrand.tracked_files(REPO_ROOT) if rebrand.LOGO_FILES.match(f)]
        self.assertGreaterEqual(len(logos), 10)
        for required in ["program_info/prismlauncher.ico", "program_info/prismlauncher.icns", "launcher/resources/multimc/scalable/launcher.svg"]:
            self.assertIn(required, logos)


class WholeTree(unittest.TestCase):
    """Runs the script on a copy of the checkout and checks the result."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        for rel in rebrand.tracked_files(REPO_ROOT):
            src = REPO_ROOT / rel
            if src.is_symlink() or not src.is_file():
                continue
            dst = cls.root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(src.read_bytes())

        report = rebrand.Report()
        files = rebrand.tracked_files(cls.root)
        edits = rebrand.targeted_edits("owner/Repo")
        if Image is not None:
            rebrand.recolor_logos(cls.root, files, report, dry_run=False)
        rebrand.apply_edits(cls.root, edits, report, dry_run=False)
        rebrand.rename_contents(cls.root, files, report, dry_run=False)
        rebrand.rename_paths(cls.root, files, report, dry_run=False)
        rebrand.add_fork_cmake(cls.root, report, dry_run=False)
        cls.report = report
        cls.problems = rebrand.audit(cls.root, edits, files)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def read(self, rel: str) -> str:
        return (self.root / rel).read_text(encoding="utf-8")

    def test_no_errors_and_clean_audit(self):
        self.assertEqual(self.report.errors, [])
        self.assertEqual(self.problems, [])

    def test_identity(self):
        info = self.read("program_info/CMakeLists.txt")
        self.assertIn('set(Launcher_DisplayName "Crism Launcher")', info)
        self.assertIn('set(Launcher_AppID "org.crismlauncher.CrismLauncher")', info)
        self.assertIn('set(Launcher_Git "https://github.com/owner/Repo")', info)
        self.assertIn("Crism Launcher Contributors\\\\n© 2022-2026 Prism Launcher Contributors", info)
        self.assertIn('set(Launcher_APP_BINARY_NAME "crismlauncher"', self.read("CMakeLists.txt"))

    def test_paths_renamed_and_old_dirs_gone(self):
        self.assertTrue((self.root / "libraries/launcher/org/crismlauncher/EntryPoint.java").is_file())
        self.assertFalse((self.root / "libraries/launcher/org/prismlauncher").exists())
        self.assertTrue((self.root / "program_info/CrismLauncher.icon/icon.json").is_file())
        self.assertFalse((self.root / "launcher/updater/prismupdater").exists())

    def test_java_entry_point_consistent(self):
        self.assertIn('"org.crismlauncher.EntryPoint"', self.read("launcher/minecraft/launch/LauncherPartLaunch.cpp"))
        self.assertIn("package org.crismlauncher;", self.read("libraries/launcher/org/crismlauncher/EntryPoint.java"))
        self.assertIn("org/crismlauncher/EntryPoint.java", self.read("libraries/launcher/CMakeLists.txt"))

    def test_kept_upstream_references(self):
        self.assertIn('.arg("Prism Launcher")', self.read("launcher/ui/dialogs/AboutDialog.cpp"))
        self.assertIn("Crism Launcher, Prism Launcher, PolyMC", self.read("launcher/ui/pages/modplatform/ImportPage.ui"))
        self.assertIn("https://meta.prismlauncher.org/v1/", self.read("CMakeLists.txt"))

    def test_skipped_files_untouched(self):
        for rel in ["COPYING.md", ".gitmodules", "flake.lock"]:
            with self.subTest(rel=rel):
                self.assertEqual((self.root / rel).read_bytes(), (REPO_ROOT / rel).read_bytes())

    def test_legacy_install_block_appended(self):
        cmake = self.read("launcher/CMakeLists.txt")
        self.assertIn("if(Launcher_LEGACY_APP_BINARY_NAME AND NOT APPLE)", cmake)
        self.assertIn("COMPONENT portable EXCLUDE_FROM_ALL", cmake)


if __name__ == "__main__":
    unittest.main()
