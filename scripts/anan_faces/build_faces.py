#!/usr/bin/env python3
"""Turn Anan's face pictures into the StackChan board's emote animations.

Two subcommands:

``prepare <originals dir> <assets-src dir>``
    Reads the 21 original pictures (``<emotion>.png``), centre-crops each to
    exactly 4:3, removes the baked-in frame by cutting 8% off every side,
    scales to 640x480 (LANCZOS) and saves a lossless PNG.  These 640x480
    files are committed as ``main/boards/m5stack/stackchan/assets-src/anan/``.

``build <assets-src dir> <board dir>``
    Scales the prepared pictures to 320x240 and writes into the board dir:
    ``emoji/anan_*.eaf``, ``assets/320_240/emote.json`` and
    ``anan_idle_actions.inc``.  Output is deterministic: the same input
    always gives byte-identical files.

Checks in ``prepare`` (a picture failing either one is rejected):

* Aspect: width/height must be within 2% of 4:3.
* Frame removed: the frame colour F is the per-channel median of the
  original's outer band (1% of the shorter side wide, all four edges; the
  median ignores the white outside the rounded corners).  After cropping,
  each of the four 6% corner squares of the 640x480 result may contain less
  than 1% of pixels whose largest channel difference from F is <= 40.  On
  the real pictures the closest corner pixel is 76 away from F at the 8%
  inset; with no inset (or a 3-5% inset) a corner holds 8-70% frame pixels.

Only Pillow and the standard library are used.
"""

import argparse
import os
import sys
from dataclasses import dataclass

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import eaf  # noqa: E402

FACE_NAMES = (
    "neutral", "happy", "laughing", "funny", "sad", "angry", "crying",
    "loving", "embarrassed", "surprised", "shocked", "thinking", "winking",
    "cool", "relaxed", "delicious", "kissy", "confident", "sleepy", "silly",
    "confused",
)

# Idle actions: (emote name, face, frames of that face).  Each plays as
# neutral x2, face x frames, neutral x2.  Add a line here to add an action.
IDLE_ACTIONS = (
    ("idle_sleepy", "sleepy", 12),
    ("idle_wink", "winking", 5),
    ("idle_delicious", "delicious", 8),
)

FPS = 5
FRAME_MS = 1000 // FPS
SCREEN_SIZE = (320, 240)
SOURCE_SIZE = (640, 480)
MAX_FILE_NAME = 31

ASPECT = 4 / 3
ASPECT_TOLERANCE = 0.02
INSET = 0.08
BORDER_BAND = 0.01      # outer band sampled for the frame colour
CORNER_PATCH = 0.06     # corner square side, fraction of output height
FRAME_DISTANCE = 40     # max channel difference that counts as frame colour
FRAME_FRACTION = 0.01   # share of frame-coloured pixels that rejects a corner


class FaceError(Exception):
    """A face picture is missing or unusable."""


@dataclass(frozen=True)
class Step:
    face: str
    count: int
    shift: int = 0  # rows moved down; top rows repeat row 0


@dataclass(frozen=True)
class Animation:
    file: str
    loop: bool
    steps: tuple

    def faces(self):
        """Distinct faces in order of first use."""
        seen = []
        for step in self.steps:
            if step.face not in seen:
                seen.append(step.face)
        return seen

    def frame_count(self):
        return sum(step.count for step in self.steps)


def emotion_animation(face):
    return Animation("anan_%s.eaf" % face, True, (
        Step(face, 8), Step(face, 2, 1), Step(face, 8, 2), Step(face, 2, 1)))


NEUTRAL_ANIMATION = Animation("anan_neutral.eaf", True, (
    Step("neutral", 18), Step("relaxed", 1), Step("neutral", 10), Step("relaxed", 1)))


def idle_animation(name, face, count):
    return Animation("anan_%s.eaf" % name, False, (
        Step("neutral", 2), Step(face, count), Step("neutral", 2)))


def emote_table():
    """Return the ordered (emote name, Animation) list for emote.json."""
    table = []
    for face in FACE_NAMES:
        table.append((face, NEUTRAL_ANIMATION if face == "neutral" else emotion_animation(face)))
    table.append(("idle", NEUTRAL_ANIMATION))
    for name, face, count in IDLE_ACTIONS:
        table.append((name, idle_animation(name, face, count)))
    return table


# ---------------------------------------------------------------- prepare

def _median_color(histogram):
    color = []
    for channel in range(3):
        counts = histogram[channel * 256:(channel + 1) * 256]
        half, running = sum(counts) / 2, 0
        for value, count in enumerate(counts):
            running += count
            if running >= half:
                color.append(value)
                break
    return tuple(color)


def frame_color(im):
    """Median colour of the outer band of the original picture."""
    w, h = im.size
    band = max(1, round(BORDER_BAND * min(w, h)))
    boxes = ((0, 0, w, band), (0, h - band, w, h), (0, band, band, h - band), (w - band, band, w, h - band))
    histogram = [0] * 768
    for box in boxes:
        for i, count in enumerate(im.crop(box).histogram()):
            histogram[i] += count
    return _median_color(histogram)


def frame_pixel_share(im, box, color):
    """Share of pixels in ``box`` within FRAME_DISTANCE of ``color``."""
    data = im.crop(box).tobytes()
    near = 0
    for i in range(0, len(data), 3):
        if (abs(data[i] - color[0]) <= FRAME_DISTANCE and abs(data[i + 1] - color[1]) <= FRAME_DISTANCE
                and abs(data[i + 2] - color[2]) <= FRAME_DISTANCE):
            near += 1
    return near / (len(data) // 3)


def crop_to_4_3(im, name):
    """Centre-crop to exact 4:3; reject pictures more than 2% off."""
    w, h = im.size
    if abs((w / h) / ASPECT - 1) > ASPECT_TOLERANCE:
        raise FaceError("%s: %dx%d is not 4:3 (allowed within %d%%)" % (name, w, h, ASPECT_TOLERANCE * 100))
    if w * 3 > h * 4:
        new_w = round(h * 4 / 3)
        left = (w - new_w) // 2
        return im.crop((left, 0, left + new_w, h))
    if w * 3 < h * 4:
        new_h = round(w * 3 / 4)
        top = (h - new_h) // 2
        return im.crop((0, top, w, top + new_h))
    return im


def prepare_image(im, name, inset=INSET):
    """Crop, inset and scale one original to 640x480; check the frame is gone."""
    im = im.convert("RGB")
    color = frame_color(im)
    im = crop_to_4_3(im, name)
    w, h = im.size
    dx, dy = round(w * inset), round(h * inset)
    out = im.crop((dx, dy, w - dx, h - dy)).resize(SOURCE_SIZE, Image.LANCZOS)
    ow, oh = out.size
    p = round(CORNER_PATCH * oh)
    for x, y in ((0, 0), (ow - p, 0), (0, oh - p), (ow - p, oh - p)):
        share = frame_pixel_share(out, (x, y, x + p, y + p), color)
        if share >= FRAME_FRACTION:
            raise FaceError("%s: frame colour %s still fills %.0f%% of the corner at (%d, %d); "
                            "the frame was not removed" % (name, color, share * 100, x, y))
    return out


def face_path(directory, name):
    path = os.path.join(directory, name + ".png")
    if not os.path.isfile(path):
        raise FaceError("%s: missing %s" % (name, path))
    return path


def prepare(src_dir, out_dir):
    """Prepare all 21 faces from ``src_dir`` into ``out_dir``."""
    paths = [(name, face_path(src_dir, name)) for name in FACE_NAMES]
    prepared = []
    for name, path in paths:
        with Image.open(path) as im:
            prepared.append((name, prepare_image(im, name)))
    os.makedirs(out_dir, exist_ok=True)
    for name, im in prepared:
        im.save(os.path.join(out_dir, name + ".png"), format="PNG")


# ------------------------------------------------------------------ build

def quantize_faces(images):
    """Quantise images to one shared 256-colour palette (no dithering).

    Returns ``(palette, pixels)``: 1024 BGRA bytes with alpha 255 (so black
    is not treated as transparent) and one index buffer per image.
    """
    w, h = images[0].size
    strip = Image.new("RGB", (w, h * len(images)))
    for i, im in enumerate(images):
        strip.paste(im, (0, i * h))
    palette_image = strip.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    rgb = list(palette_image.getpalette()[:768])
    rgb += [0] * (768 - len(rgb))
    palette = bytearray()
    for i in range(256):
        r, g, b = rgb[i * 3:i * 3 + 3]
        palette += bytes([b, g, r, 255])
    pixels = [im.quantize(palette=palette_image, dither=Image.Dither.NONE).tobytes() for im in images]
    return bytes(palette), pixels


def shift_down(pixels, width, rows):
    """Move the picture down by ``rows``; the top repeats row 0."""
    if rows == 0:
        return pixels
    return pixels[:width] * rows + pixels[:len(pixels) - rows * width]


def load_source(src_dir, name):
    with Image.open(face_path(src_dir, name)) as im:
        if im.size != SOURCE_SIZE:
            raise FaceError("%s: source is %dx%d, expected %dx%d (run prepare)" % ((name,) + im.size + SOURCE_SIZE))
        return im.convert("RGB").resize(SCREEN_SIZE, Image.LANCZOS)


def animation_frames(animation, sources):
    """Encode the frame sequence of one animation."""
    faces = animation.faces()
    palette, pixels = quantize_faces([sources[face] for face in faces])
    indices = dict(zip(faces, pixels))
    w, h = SCREEN_SIZE
    cache = {}
    frames = []
    for step in animation.steps:
        key = (step.face, step.shift)
        if key not in cache:
            cache[key] = eaf.build_frame(w, h, palette, shift_down(indices[step.face], w, step.shift))
        frames += [cache[key]] * step.count
    return frames


def emote_json(table):
    lines = ['    {"emote": "%s", "src": "%s", "loop": %s, "fps": %d}'
             % (name, anim.file, "true" if anim.loop else "false", FPS) for name, anim in table]
    return "[\n" + ",\n".join(lines) + "\n]\n"


def idle_actions_inc():
    lines = ["// Generated by scripts/anan_faces/build_faces.py; do not edit."]
    for name, face, count in IDLE_ACTIONS:
        frames = idle_animation(name, face, count).frame_count()
        lines.append('ANAN_IDLE_ACTION("%s", %d)' % (name, frames * FRAME_MS))
    return "\n".join(lines) + "\n"


def write_text(path, text):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write(text)


def build(src_dir, board_dir):
    """Generate every animation and config file from the prepared faces."""
    sources = {name: load_source(src_dir, name) for name in FACE_NAMES}
    table = emote_table()
    animations = {}
    for _, anim in table:
        if len(anim.file) > MAX_FILE_NAME:
            raise FaceError("%s: file name longer than %d characters" % (anim.file, MAX_FILE_NAME))
        animations[anim.file] = anim

    emoji_dir = os.path.join(board_dir, "emoji")
    config_dir = os.path.join(board_dir, "assets", "320_240")
    os.makedirs(emoji_dir, exist_ok=True)
    os.makedirs(config_dir, exist_ok=True)
    for old in sorted(os.listdir(emoji_dir)):
        if old.startswith("anan_") and old.endswith(".eaf") and old not in animations:
            os.remove(os.path.join(emoji_dir, old))

    sizes = {}
    for file_name in sorted(animations):
        frames = animation_frames(animations[file_name], sources)
        sizes[file_name] = eaf.write(os.path.join(emoji_dir, file_name), frames)
    write_text(os.path.join(config_dir, "emote.json"), emote_json(table))
    write_text(os.path.join(board_dir, "anan_idle_actions.inc"), idle_actions_inc())
    return sizes


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare", help="crop originals to 640x480 sources")
    p.add_argument("originals")
    p.add_argument("assets_src")
    b = sub.add_parser("build", help="generate .eaf files, emote.json and the idle action list")
    b.add_argument("assets_src")
    b.add_argument("board_dir")
    args = parser.parse_args(argv)
    try:
        if args.command == "prepare":
            prepare(args.originals, args.assets_src)
            print("prepared %d faces into %s" % (len(FACE_NAMES), args.assets_src))
        else:
            sizes = build(args.assets_src, args.board_dir)
            for name in sorted(sizes):
                print("%-28s %9d bytes" % (name, sizes[name]))
            print("%d files, %d bytes total" % (len(sizes), sum(sizes.values())))
    except FaceError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
