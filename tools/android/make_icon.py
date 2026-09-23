#!/usr/bin/env python3
"""Build a launcher icon for a port from the title's own artwork.

A launcher on Android 8 or later given only a legacy bitmap applies the legacy
icon treatment: it shrinks the picture, centres it on a white plate and masks
that, which is why a port shipped with one PNG shows a postage stamp in a white
circle. There is no way to opt out - the only fix is to ship the adaptive
layers, so that is what this writes.

Both layers are 108dp square and only the central 72dp survives the mask:

  background  the artwork, cover-fit and bleeding off all four edges, then
              blurred and darkened. Blurred because the foreground is the same
              picture: sharp, the icon reads as one badge drawn twice. Cover-fit
              because contain-fit leaves a gap, and the gap comes back as the
              launcher's plate colour - the white border again by another route.
  foreground  the artwork again, fitted inside the 72dp safe zone, so nothing
              recognisable is cropped by whatever mask the launcher applies.

    make_icon.py <artwork.png> <res-dir>
"""
import sys
from pathlib import Path

from PIL import Image, ImageEnhance, ImageFilter

# 108dp at xxxhdpi. One size is enough: Android scales a drawable down far
# better than it invents detail scaling one up.
CANVAS = 432
SAFE = round(CANVAS * 2 / 3)  # the central 72dp of 108dp


def cover(image: Image.Image, size: int) -> Image.Image:
    """Scaled to fill `size` square, cropping whatever overflows."""
    scale = max(size / image.width, size / image.height)
    scaled = image.resize(
        (max(1, round(image.width * scale)), max(1, round(image.height * scale))),
        Image.LANCZOS,
    )
    left = (scaled.width - size) // 2
    top = (scaled.height - size) // 2
    return scaled.crop((left, top, left + size, top + size))


def contain(image: Image.Image, size: int) -> Image.Image:
    """Scaled to fit inside `size` square, keeping all of it."""
    scale = min(size / image.width, size / image.height)
    return image.resize(
        (max(1, round(image.width * scale)), max(1, round(image.height * scale))),
        Image.LANCZOS,
    )


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    source = Path(sys.argv[1])
    res = Path(sys.argv[2])
    if not source.is_file():
        print(f"make_icon: no artwork at {source}", file=sys.stderr)
        return 1

    art = Image.open(source).convert("RGBA")

    drawable = res / "drawable"
    anydpi = res / "drawable-anydpi-v26"
    drawable.mkdir(parents=True, exist_ok=True)
    anydpi.mkdir(parents=True, exist_ok=True)

    background = cover(art, CANVAS).convert("RGB")
    background = background.filter(ImageFilter.GaussianBlur(CANVAS / 12))
    background = ImageEnhance.Brightness(background).enhance(0.7)
    background.save(drawable / "ic_launcher_background.png")

    foreground = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    fitted = contain(art, SAFE)
    foreground.paste(fitted, ((CANVAS - fitted.width) // 2, (CANVAS - fitted.height) // 2), fitted)
    foreground.save(drawable / "ic_launcher_foreground.png")

    # The legacy bitmap, for anything older than API 26. Same resource name, so
    # the manifest does not care which one a device resolves.
    cover(art, 192).convert("RGB").save(drawable / "ic_launcher.png")

    (anydpi / "ic_launcher.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@drawable/ic_launcher_background" />\n'
        '    <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
        "</adaptive-icon>\n",
        encoding="utf-8",
    )
    print(f"make_icon: wrote adaptive + legacy launcher icon from {source.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
