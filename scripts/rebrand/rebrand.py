#!/usr/bin/env python3
"""Rebrand a Prism Launcher checkout as Crism Launcher.

CI runs this on its fresh checkout before configuring, so the repository itself keeps upstream's names and the
daily upstream merge stays conflict-free. The rules, what stays untouched and why, are in context/branding.md.

    python3 scripts/rebrand/rebrand.py [--root DIR] [--repo OWNER/NAME] [--dry-run]
    python3 scripts/rebrand/rebrand.py --readme-logo OUT.svg    (only writes the recolored icon, for the README)

Order matters:
    recolor logos -> targeted edits -> rename contents -> rename paths -> fork CMake additions -> audit
The audit fails the run if anything named prism survives outside the protected spans, or if a targeted edit no
longer matches upstream's text.
"""

from __future__ import annotations

import argparse
import colorsys
import io
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Hue band the logo colours are squeezed into: magenta through crimson to orange-red. Keeping the original hue order
# inside the band keeps the facets of the prism distinguishable.
HUE_BAND_START = 300 / 360
HUE_BAND_WIDTH = 80 / 360

DEFAULT_REPO = "coffeegrind123/CrismLauncher"

# The app icon without the wordmark: the wordmark logos spell "Prism Launcher" in outlined glyphs, which no text
# rename can change
README_LOGO_SOURCE = "program_info/org.prismlauncher.PrismLauncher.svg"

# Wraps text that a targeted edit inserts and the rename must leave alone (e.g. a reference to upstream Prism)
KEEP_OPEN = b"\x01"
KEEP_CLOSE = b"\x02"

# Not rewritten at all: workflows are already loaded by the time this runs and name upstream on purpose, licence
# texts carry upstream's notices verbatim, test data is captured from the outside world, and the vcpkg overlay ports
# feed vcpkg's ABI hash (a changed comment would invalidate the binary cache)
SKIP_CONTENT = (
    re.compile(r"^\.github/workflows/"),
    re.compile(r"^\.gitmodules$"),
    re.compile(r"^flake\.lock$"),
    re.compile(r"^(COPYING\.md|LICENSE)$"),
    re.compile(r"(^|/)LICENSE$"),
    re.compile(r"^tests/testdata/"),
    re.compile(r"^cmake/vcpkg-ports/"),
    re.compile(r"^scripts/rebrand/"),
)

# Paths that end up in the build or the packages; the audit is limited to these
AUDIT_SCOPE = re.compile(
    r"^(CMakeLists\.txt|buildconfig/|cmake/(?!vcpkg-ports/)|launcher/|libraries/launcher/|program_info/|tests/(?!testdata/)"
    r"|\.github/actions/)"
)

# Spans that keep "prism", matched before the rename. Each is a reason, not a convenience:
PROTECT = [
    # Attribution: copyright and contributor lines credit upstream's authors (GPL-3.0 sections 4 and 5)
    re.compile(rb"(?im)^.*(?:copyright|spdx-filecopyrighttext|contributors).*$"),
    # Upstream's live services: version metadata, translations, legacy FML libraries, website, nix cache, Weblate,
    # donations. There is no Crism equivalent and renaming them breaks the launcher.
    re.compile(rb"(?i)(?:[a-z0-9-]+\.)*prismlauncher\.org"),
    re.compile(rb"(?i)prismlauncher\.cachix\.org"),
    re.compile(rb"(?i)hosted\.weblate\.org/projects/prismlauncher"),
    re.compile(rb"(?i)opencollective\.com/prismlauncher"),
    # Links into upstream's repositories (issue references, the libnbtplusplus submodule, the contributors page)
    re.compile(rb"(?i)github\.com/PrismLauncher/[A-Za-z0-9._/#?=&-]*"),
    re.compile(rb"(?i)github:PrismLauncher/[A-Za-z0-9._/-]*"),
    # Not the brand: PrismarineJS/prismarine-auth is credited in the Xbox auth code
    re.compile(rb"(?i)prismarine[A-Za-z0-9_-]*"),
    # File format keys: packwiz .pw.toml files written by any Prism-family launcher must keep loading both ways
    re.compile(rb"(?i)x-prismlauncher-[a-z0-9-]+"),
    # Update hand-off: an install from before the rename launches the new updater out of the unpacked package, and
    # that updater only finishes the install if it finds the marker under the name the old updater wrote
    re.compile(rb"\.prism_launcher_updater_unpack\.marker"),
    # prismlauncher:// import links come from third-party sites; upstream accepts that scheme in every fork
    re.compile(rb'scheme\(\) == "prismlauncher"'),
]

KEEP_SPAN = re.compile(re.escape(KEEP_OPEN) + rb".*?" + re.escape(KEEP_CLOSE), re.S)
PRISM = re.compile(rb"(?i)prism")
PLACEHOLDER = re.compile(rb"\x00(\d+)\x00")


def keep(text: str) -> bytes:
    return KEEP_OPEN + text.encode() + KEEP_CLOSE


@dataclass
class Edit:
    """A targeted replacement applied before the generic rename. It must match, so upstream drift fails loudly."""

    path: str
    old: bytes
    new: bytes
    count: int = 1


def targeted_edits(repo: str) -> list[Edit]:
    fork = f"https://github.com/{repo}".encode()
    upstream = b"https://github.com/PrismLauncher/PrismLauncher"
    return [
        # Links that should lead to the fork: source code, bug tracker, update source
        Edit("program_info/CMakeLists.txt", b'set(Launcher_Git "' + upstream + b'")', b'set(Launcher_Git "' + fork + b'")'),
        Edit("CMakeLists.txt", b'"' + upstream + b'/issues" CACHE STRING "URL for the bug tracker."', b'"' + fork + b'/issues" CACHE STRING "URL for the bug tracker."'),
        Edit("CMakeLists.txt", b'set(Launcher_UPDATER_GITHUB_REPO "' + upstream + b'"', b'set(Launcher_UPDATER_GITHUB_REPO "' + fork + b'"'),
        Edit("program_info/org.prismlauncher.PrismLauncher.metainfo.xml.in", b'<url type="bugtracker">' + upstream + b"/issues</url>", b'<url type="bugtracker">' + fork + b"/issues</url>"),
        Edit("program_info/org.prismlauncher.PrismLauncher.metainfo.xml.in", b'<url type="vcs-browser">' + upstream + b"</url>", b'<url type="vcs-browser">' + fork + b"</url>"),
        Edit("program_info/org.prismlauncher.PrismLauncher.metainfo.xml.in", b'<url type="contribute">' + upstream + b"/blob/develop/CONTRIBUTING.md</url>", b'<url type="contribute">' + fork + b"/blob/develop/CONTRIBUTING.md</url>"),
        # Credit Crism's contributors alongside upstream's, which stay as they are
        Edit("program_info/CMakeLists.txt", b'set(Launcher_Copyright "\xc2\xa9 ', b'set(Launcher_Copyright "\xc2\xa9 2025-2026 Crism Launcher Contributors\\\\n\xc2\xa9 '),
        Edit("program_info/CMakeLists.txt", b'set(Launcher_Copyright_Mac "\xc2\xa9 ', b'set(Launcher_Copyright_Mac "\xc2\xa9 2025-2026 Crism Launcher Contributors, \xc2\xa9 '),
        Edit("program_info/CMakeLists.txt", b'set(Launcher_Authors "MultiMC & Prism Launcher Contributors")', b'set(Launcher_Authors "MultiMC, Prism Launcher & Crism Launcher Contributors")'),
        # The credits list upstream's developers under a heading built from the launcher's name
        Edit("launcher/ui/dialogs/AboutDialog.cpp", b'QObject::tr("%1 Developers").arg(BuildConfig.LAUNCHER_DISPLAYNAME)', b'QObject::tr("%1 Developers").arg("' + keep("Prism Launcher") + b'")'),
        # Instances exported by upstream Prism still import; say so
        Edit("launcher/ui/pages/modplatform/ImportPage.ui", b"- Prism Launcher, PolyMC or MultiMC exported instances (ZIP)", b"- Crism Launcher, " + keep("Prism Launcher") + b", PolyMC or MultiMC exported instances (ZIP)"),
    ]


# Installed into portable packages only. A pre-rename install updates by unpacking the new package and launching
# <old binary name>_updater from it, so the package has to contain that file; old shortcuts and the old portable
# start script call <old binary name> directly. Both are copies of the renamed binaries, taken after the main
# install so the RPATH is the installed one. Launcher_LEGACY_APP_BINARY_NAME is set by CI.
LEGACY_INSTALL_CMAKE = """
# Added by scripts/rebrand/rebrand.py: copies of the binaries under their pre-rename names
if(Launcher_LEGACY_APP_BINARY_NAME AND NOT APPLE)
    set(_legacy_bin "${BINARY_DEST_DIR}")
    set(_legacy_names "${Launcher_APP_BINARY_NAME}=${Launcher_LEGACY_APP_BINARY_NAME}")
    if(TARGET "${Launcher_Name}_updater")
        list(APPEND _legacy_names "${Launcher_APP_BINARY_NAME}_updater=${Launcher_LEGACY_APP_BINARY_NAME}_updater")
    endif()
    foreach(_pair IN LISTS _legacy_names)
        string(REPLACE "=" ";" _pair "${_pair}")
        list(GET _pair 0 _from)
        list(GET _pair 1 _to)
        install(CODE "
            set(_dir \\"\\${CMAKE_INSTALL_PREFIX}/${_legacy_bin}\\")
            file(COPY_FILE \\"\\${_dir}/${_from}${CMAKE_EXECUTABLE_SUFFIX}\\" \\"\\${_dir}/${_to}${CMAKE_EXECUTABLE_SUFFIX}\\")
            message(STATUS \\"Legacy name: \\${_dir}/${_to}${CMAKE_EXECUTABLE_SUFFIX}\\")
        " COMPONENT portable EXCLUDE_FROM_ALL)
    endforeach()
endif()
"""

# Logo artwork to recolor, by pre-rename path. Instance icons that merely carry Prism's metadata are not logos.
LOGO_FILES = re.compile(
    r"^(program_info/(org\.prismlauncher\.PrismLauncher[^/]*\.(svg|png)|prismlauncher\.(ico|icns)|PrismLauncher\.icon/Assets/[^/]+\.svg)"
    r"|launcher/resources/multimc/scalable/(launcher|instances/prismlauncher)\.svg)$"
)
SVG_COLOR = re.compile(
    rb"((?:fill|stroke|stop-color|flood-color|lighting-color|color)\s*[:=]\s*[\"']?\s*)#([0-9a-fA-F]{6}|[0-9a-fA-F]{3})\b"
)


@dataclass
class Report:
    recolored: list[str] = field(default_factory=list)
    edited: list[str] = field(default_factory=list)
    renamed_paths: list[tuple[str, str]] = field(default_factory=list)
    replacements: int = 0
    errors: list[str] = field(default_factory=list)


def rename_token(match: re.Match[bytes]) -> bytes:
    """prism -> crism, keeping the case of every letter (Prism, PRISM, PrismLauncher, m_prismVersion)."""
    word = match.group(0)
    return (b"C" if word[:1] == b"P" else b"c") + word[1:]


def rename_text(data: bytes) -> tuple[bytes, int]:
    """Renames every prism outside the protected spans and strips the keep markers."""
    saved: list[bytes] = []

    def stash(match: re.Match[bytes]) -> bytes:
        saved.append(match.group(0).replace(KEEP_OPEN, b"").replace(KEEP_CLOSE, b""))
        return b"\x00%d\x00" % (len(saved) - 1)

    data = KEEP_SPAN.sub(stash, data)
    for pattern in PROTECT:
        data = pattern.sub(stash, data)

    data, count = PRISM.subn(rename_token, data)
    return PLACEHOLDER.sub(lambda m: saved[int(m.group(1))], data), count


def leftovers(data: bytes, allowed: list[bytes] = ()) -> list[int]:
    """Line numbers where prism survives outside the protected spans and the allowed texts."""
    masked = data
    for text in allowed:
        masked = masked.replace(text, re.sub(rb"[^\n]", b"_", text))
    for pattern in PROTECT:
        masked = pattern.sub(lambda m: re.sub(rb"[^\n]", b"_", m.group(0)), masked)
    return sorted({masked.count(b"\n", 0, m.start()) + 1 for m in PRISM.finditer(masked)})


def is_binary(data: bytes) -> bool:
    return b"\x00" in data[:8192]


def rename_path(path: str) -> str:
    renamed, _ = rename_text(path.encode())
    return renamed.decode()


def shift_hue(h: float) -> float:
    return (HUE_BAND_START + h * HUE_BAND_WIDTH) % 1.0


def recolor_hex(hex_digits: bytes) -> bytes:
    digits = hex_digits.decode()
    if len(digits) == 3:
        digits = "".join(c * 2 for c in digits)
    r, g, b = (int(digits[i : i + 2], 16) / 255 for i in (0, 2, 4))
    h, l, s = colorsys.rgb_to_hls(r, g, b)
    r, g, b = colorsys.hls_to_rgb(shift_hue(h), l, s)
    return ("%02x%02x%02x" % tuple(round(c * 255) for c in (r, g, b))).encode()


def recolor_svg(data: bytes) -> bytes:
    return SVG_COLOR.sub(lambda m: m.group(1) + b"#" + recolor_hex(m.group(2)), data)


def recolor_image(image):
    """Hue-shifts an image in place of its alpha; hue is the same quantity in HLS and HSV, so this matches the SVGs."""
    from PIL import Image

    rgba = image.convert("RGBA")
    alpha = rgba.getchannel("A")
    h, s, v = rgba.convert("RGB").convert("HSV").split()
    h = h.point([round(shift_hue(i / 256) * 256) % 256 for i in range(256)])
    out = Image.merge("HSV", (h, s, v)).convert("RGB").convert("RGBA")
    out.putalpha(alpha)
    return out


def recolor_raster(path: Path) -> None:
    from PIL import Image

    suffix = path.suffix.lower()
    with Image.open(path) as im:
        if suffix == ".png":
            recolor_image(im).save(path, format="PNG", optimize=True)
            return

        if suffix == ".ico":
            sizes = sorted(im.ico.sizes(), reverse=True)
            frames = [recolor_image(im.ico.getimage(size)) for size in sizes]
        elif suffix == ".icns":
            # (width, height, scale) entries; several can share a pixel size, ICNS stores one per pixel size
            by_pixels = {(w * s, h * s): (w, h, s) for w, h, s in im.info["sizes"]}
            frames = [recolor_image(im.icns.getimage(by_pixels[p])) for p in sorted(by_pixels, reverse=True)]
        else:
            raise ValueError(f"unsupported image {path}")

    out = io.BytesIO()
    frames[0].save(out, format=suffix[1:].upper(), append_images=frames[1:], sizes=[f.size for f in frames])
    path.write_bytes(out.getvalue())


def tracked_files(root: Path) -> list[str]:
    if (root / ".git").exists():
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True, capture_output=True).stdout
        return [p for p in out.decode().split("\0") if p]
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for name in filenames:
            files.append(Path(dirpath, name).relative_to(root).as_posix())
    return sorted(files)


def is_regular(path: Path) -> bool:
    return path.is_file() and not path.is_symlink()


def recolor_logos(root: Path, files: list[str], report: Report, dry_run: bool) -> None:
    for rel in files:
        if not LOGO_FILES.match(rel):
            continue
        path = root / rel
        if not is_regular(path):
            continue
        report.recolored.append(rel)
        if dry_run:
            continue
        if path.suffix == ".svg":
            path.write_bytes(recolor_svg(path.read_bytes()))
        else:
            recolor_raster(path)


def apply_edits(root: Path, edits: list[Edit], report: Report, dry_run: bool) -> None:
    for edit in edits:
        path = root / edit.path
        data = path.read_bytes() if path.is_file() else b""
        found = data.count(edit.old)
        if found != edit.count:
            report.errors.append(f"{edit.path}: targeted edit expected {edit.count} match(es), found {found}: {edit.old[:90]!r}")
            continue
        report.edited.append(edit.path)
        if not dry_run:
            path.write_bytes(data.replace(edit.old, edit.new))


def rename_contents(root: Path, files: list[str], report: Report, dry_run: bool) -> None:
    for rel in files:
        if any(p.search(rel) for p in SKIP_CONTENT):
            continue
        path = root / rel
        if not is_regular(path):
            continue
        data = path.read_bytes()
        if is_binary(data) or (KEEP_OPEN not in data and not PRISM.search(data)):
            continue
        renamed, count = rename_text(data)
        report.replacements += count
        if not dry_run and renamed != data:
            path.write_bytes(renamed)


def rename_paths(root: Path, files: list[str], report: Report, dry_run: bool) -> None:
    for rel in files:
        if any(p.search(rel) for p in SKIP_CONTENT) or not PRISM.search(rel.encode()):
            continue
        new_rel = rename_path(rel)
        if new_rel == rel:
            continue
        report.renamed_paths.append((rel, new_rel))
        if dry_run:
            continue
        src, dst = root / rel, root / new_rel
        if not (src.exists() or src.is_symlink()):
            report.errors.append(f"{rel}: listed but missing")
            continue
        if dst.exists():
            report.errors.append(f"{rel}: rename target {new_rel} already exists")
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        os.replace(src, dst)

    # Drop the directories the moves emptied (deepest first)
    if not dry_run:
        old_dirs = {Path(old).parent for old, _ in report.renamed_paths}
        for directory in sorted({p for d in old_dirs for p in [d, *d.parents]}, key=lambda p: len(p.parts), reverse=True):
            full = root / directory
            if directory != Path(".") and full.is_dir() and not any(full.iterdir()):
                full.rmdir()


def add_fork_cmake(root: Path, report: Report, dry_run: bool) -> None:
    path = root / "launcher/CMakeLists.txt"
    if not path.is_file():
        report.errors.append("launcher/CMakeLists.txt: missing, cannot add legacy binary names")
        return
    if not dry_run:
        with path.open("ab") as f:
            f.write(LEGACY_INSTALL_CMAKE.encode())


def audit(root: Path, edits: list[Edit]) -> list[str]:
    """Walks the tree rather than asking git, which still lists the pre-rename paths. Text a targeted edit kept on
    purpose is allowed, in its final form."""
    allowed: dict[str, list[bytes]] = {}
    for edit in edits:
        if KEEP_OPEN in edit.new:
            allowed.setdefault(rename_path(edit.path), []).append(rename_text(edit.new)[0])
    problems = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for name in filenames:
            path = Path(dirpath, name)
            rel = path.relative_to(root).as_posix()
            if not AUDIT_SCOPE.match(rel) or any(p.search(rel) for p in SKIP_CONTENT):
                continue
            if leftovers(rel.encode()):
                problems.append(f"{rel}: path still contains prism")
            if not is_regular(path):
                continue
            data = path.read_bytes()
            if is_binary(data):
                continue
            for line in leftovers(data, allowed.get(rel, [])):
                problems.append(f"{rel}:{line}: {data.splitlines()[line - 1].decode(errors='replace').strip()[:160]}")
    return problems


def list_protected(root: Path, files: list[str]) -> None:
    for rel in files:
        path = root / rel
        if not AUDIT_SCOPE.match(rel) or not is_regular(path):
            continue
        data = path.read_bytes()
        if is_binary(data):
            continue
        for pattern in PROTECT:
            for match in pattern.finditer(data):
                if PRISM.search(match.group(0)):
                    line = data.count(b"\n", 0, match.start()) + 1
                    print(f"protected {rel}:{line}: {match.group(0).decode(errors='replace').strip()[:140]}")


def write_readme_logo(root: Path, out: Path) -> None:
    logo, _ = rename_text(recolor_svg((root / README_LOGO_SOURCE).read_bytes()))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(logo)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY") or DEFAULT_REPO, help="fork's GitHub OWNER/NAME")
    parser.add_argument("--dry-run", action="store_true", help="report what would change without writing")
    parser.add_argument("--list-protected", action="store_true", help="print every span that keeps prism, for review")
    parser.add_argument("--readme-logo", type=Path, metavar="OUT", help="write the recolored icon to OUT and stop")
    args = parser.parse_args()

    root = args.root.resolve()
    if args.readme_logo:
        write_readme_logo(root, args.readme_logo)
        print(f"Wrote {args.readme_logo}")
        return 0

    files = tracked_files(root)
    report = Report()
    edits = targeted_edits(args.repo)

    recolor_logos(root, files, report, args.dry_run)
    apply_edits(root, edits, report, args.dry_run)
    rename_contents(root, files, report, args.dry_run)
    rename_paths(root, files, report, args.dry_run)
    add_fork_cmake(root, report, args.dry_run)

    print(f"Recolored {len(report.recolored)} logo files")
    print(f"Applied {len(report.edited)} targeted edits")
    print(f"Replaced {report.replacements} occurrences of prism")
    print(f"Renamed {len(report.renamed_paths)} paths")
    for old, new in report.renamed_paths:
        print(f"  {old} -> {new}")

    if args.list_protected:
        list_protected(root, files)

    if not args.dry_run:
        report.errors += audit(root, edits)

    if report.errors:
        print(f"\n{len(report.errors)} problem(s):", file=sys.stderr)
        for error in report.errors:
            print(f"  {error}", file=sys.stderr)
            if os.environ.get("GITHUB_ACTIONS"):
                print(f"::error title=Rebrand::{error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
