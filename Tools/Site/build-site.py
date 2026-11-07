#!/usr/bin/env python3
"""Build the project page (GitHub Pages) from files already in the repository.

The page takes the version from Include/Bifrost/Version.hpp and, from README.md, the
highlights, the "What runs" summary and table, and the screenshots; each table row's
status is checked against Scripts/Android/compatibility.json, and the build fails rather
than publish a page that disagrees with it. Writes index.html, sitemap.xml (submitted in Search
Console: a project page's robots.txt is not read, only the domain root's) and the screenshots
(WebP, and a JPEG link preview; needs Pillow) to the output directory (default build/site), with
the Search Console verification files kept beside this script (google<token>.html, unchanged).

Environment: SITE_URL (default from GITHUB_REPOSITORY, else the project's Pages address).
"""
import argparse
import datetime
import html
import json
import os
from pathlib import Path
import re
import shutil
import string

ROOT = Path(__file__).resolve().parents[2]
REPO = "https://github.com/sloppyman2567/bifrost-emu"
DESCRIPTION = ("Free, open-source ARM64 emulator for x86_64 Linux: run Android games (APKs) and AArch64 Linux "
               "apps with a JIT, a real Android 15 runtime and your own GPU.")


def site_url():
    if os.environ.get("SITE_URL"):
        return os.environ["SITE_URL"].rstrip("/") + "/"
    owner, _, name = os.environ.get("GITHUB_REPOSITORY", "sloppyman2567/bifrost-emu").partition("/")
    return f"https://{owner.lower()}.github.io/{name}/"


def inline(text):
    """README inline Markdown (code, bold, italics, links) as HTML; relative links point into the repository."""
    out, at = [], 0
    for match in re.finditer(r"`([^`]+)`|\*\*(.+?)\*\*|\*([^*]+)\*|\[([^\]]+)\]\(([^)]+)\)", text):
        out.append(html.escape(text[at:match.start()], quote=False))
        code, bold, italic, label, target = match.groups()
        if code is not None:
            out.append(f"<code>{html.escape(code, quote=False)}</code>")
        elif bold is not None:
            out.append(f"<strong>{inline(bold)}</strong>")
        elif italic is not None:
            out.append(f"<em>{inline(italic)}</em>")
        else:
            if target.startswith("#"):
                target = REPO + target
            elif not re.match(r"[a-z]+://", target):
                target = f"{REPO}/blob/main/{target}"
            out.append(f'<a href="{html.escape(target)}">{inline(label)}</a>')
        at = match.end()
    out.append(html.escape(text[at:], quote=False))
    return "".join(out)


def plain(text):
    return re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text).replace("**", "").replace("`", "")


def section(readme, title):
    match = re.search(rf"^## {re.escape(title)}\n(.*?)(?=^## )", readme, re.S | re.M)
    if not match:
        raise SystemExit(f"README.md has no '## {title}' section")
    return match.group(1)


def features(readme):
    items = []
    for bullet in re.split(r"\n(?=- )", section(readme, "Highlights").strip()):
        text = " ".join(line.strip() for line in bullet.lstrip("- ").splitlines())
        match = re.match(r"\*\*(.+?)\*\* — (.+)", text)
        if not match:
            raise SystemExit(f"README highlight not understood: {text[:60]}")
        description = match.group(2)
        description = description[:1].upper() + description[1:]  # the README continues a sentence after the dash
        items.append(f"    <div><h3>{inline(match.group(1))}</h3><p>{inline(description)}</p></div>")
    return "\n".join(items)


def normalized(name):
    name = re.sub(r"\(.*?\)", "", name.lower())
    return " ".join(re.sub(r"[^a-z0-9 ]+", " ", name).split())


def status_of(text):
    """The row's status class: playable, blocked, tested (test suites) or starts."""
    lowered = text.lower()
    if lowered.startswith("blocked"):
        return "blocked"
    if re.search(r"user-(confirmed|reported)( stable)? (gameplay|playable|play|cooking|extended)", lowered):
        return "playable"
    if lowered.startswith(("covered by", "crc-validated")):
        return "tested"
    return "starts"


LABELS = {"playable": "Playable", "blocked": "Blocked", "tested": "Tested", "starts": "Starts"}


def games(readme, compat):
    body = section(readme, "What runs")
    summary = re.search(r"\*\*(\d+) games recorded \((\d{4}-\d{2}-\d{2})\): (\d+) playable\*\*.*?(?=\n\n)", body, re.S)
    if not summary:
        raise SystemExit("README 'What runs' summary not understood")
    rows, statuses = [], []
    table = re.search(r"^\| Program \| Kind \| Status \|\n\|[-|]+\|\n((?:\|.*\|\n)+)", body, re.M)
    if not table:
        raise SystemExit("README 'What runs' table not found")
    for line in table.group(1).splitlines():
        program, kind, status = (cell.strip() for cell in line.strip("|").split(" | "))
        state = status_of(status)
        statuses.append((normalized(plain(program)), state))
        rows.append(f'      <tr><td class="name">{inline(program)}</td><td class="kind">{inline(kind)}</td>'
                    f'<td><span class="pill {state}">{LABELS[state]}</span></td><td class="note">{inline(status)}</td></tr>')
    # Every playable or blocked app on record has a row, and the row agrees with it.
    for package, app in compat["apps"].items():
        if app["status"] not in ("playable", "blocked"):
            continue
        name = normalized(app["name"])
        found = [state for row, state in statuses if name in row]
        if not found:
            raise SystemExit(f"README 'What runs' has no row for {app['name']} ({package}, {app['status']})")
        if app["status"] == "playable" and "playable" not in found:
            raise SystemExit(f"README marks {app['name']} {found[0]}, compatibility.json playable")
        if app["status"] == "blocked" and found[0] not in ("blocked", "playable"):
            raise SystemExit(f"README marks {app['name']} {found[0]}, compatibility.json blocked")
    text = " ".join(summary.group(0).split())
    return summary, inline(text), "\n".join(rows)


def screenshots(readme, output):
    """The README's screenshots as WebP at most 960 px wide (twice their width on the page), and the first one as a
    1200 px JPEG link preview, the format every social card reader takes."""
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("build-site.py needs Pillow (python3 -m pip install pillow)")
    shots, preview = [], None
    images = output / "images"
    images.mkdir(parents=True, exist_ok=True)
    for src, alt, caption in re.findall(r'<img src="([^"]+)" alt="([^"]+)"[^>]*><br><sub>([^<]+)</sub>',
                                        section(readme, "Screenshots")):
        source = ROOT / src
        stem = source.parent.name.lower() + "-" + source.stem
        with Image.open(source) as image:
            image = image.convert("RGB")
            if preview is None:
                card = image.copy()
                card.thumbnail((1200, 1200))
                card.save(images / "preview.jpg", quality=85, optimize=True, progressive=True)
                preview = "images/preview.jpg"
            image.thumbnail((960, 960))
            image.save(images / f"{stem}.webp", quality=80, method=6)
            width, height = image.size
        shots.append(f'    <figure><img src="images/{stem}.webp" alt="{html.escape(alt)}" width="{width}" '
                     f'height="{height}" loading="lazy" decoding="async"><figcaption>{html.escape(caption)}'
                     f"</figcaption></figure>")
    if not shots:
        raise SystemExit("README has no screenshots")
    return "\n".join(shots), preview


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, default=ROOT / "build/site")
    args = parser.parse_args()
    readme = (ROOT / "README.md").read_text()
    compat = json.loads((ROOT / "Scripts/Android/compatibility.json").read_text())
    version = re.search(r'VERSION\s*=\s*"([^"]+)"', (ROOT / "Include/Bifrost/Version.hpp").read_text()).group(1)
    url = site_url()
    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)
    summary, summary_html, rows = games(readme, compat)
    shots, first_shot = screenshots(readme, args.output)
    json_ld = json.dumps({
        "@context": "https://schema.org", "@type": "SoftwareApplication", "name": "Bifrost",
        "alternateName": "bifrost-emu", "applicationCategory": "UtilitiesApplication", "operatingSystem": "Linux",
        "softwareVersion": version, "description": DESCRIPTION, "url": url, "downloadUrl": REPO,
        "codeRepository": REPO, "license": "https://unlicense.org/", "isAccessibleForFree": True,
        "offers": {"@type": "Offer", "price": "0", "priceCurrency": "USD"},
    }, separators=(",", ":")).replace("</", "<\\/")
    page = string.Template((Path(__file__).with_name("page.html")).read_text()).substitute(
        description=html.escape(DESCRIPTION), url=url, repo=REPO, og_image=url + first_shot, json_ld=json_ld,
        version=html.escape(version), games=summary.group(1), playable=summary.group(3), checked=summary.group(2),
        summary=summary_html, features=features(readme), rows=rows, shots=shots)
    (args.output / "index.html").write_text(page)
    # Search Console's HTML-file verification: served as downloaded, next to the page.
    for verification in sorted(Path(__file__).parent.glob("google*.html")):
        if not re.fullmatch(r"google[0-9a-f]{16}\.html", verification.name) or \
                verification.read_text().strip() != f"google-site-verification: {verification.name}":
            raise SystemExit(f"{verification.name} is not a Search Console verification file")
        shutil.copyfile(verification, args.output / verification.name)
    today = datetime.date.today().isoformat()
    (args.output / "sitemap.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
        f"  <url><loc>{url}</loc><lastmod>{today}</lastmod></url>\n</urlset>\n")
    print(f"Wrote {args.output.relative_to(ROOT) if args.output.is_relative_to(ROOT) else args.output} for {url}")


if __name__ == "__main__":
    main()
