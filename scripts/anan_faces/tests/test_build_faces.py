"""Tests for build_faces.py (prepare and build)."""

import filecmp
import json
import os
import sys
import tempfile
import unittest

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import build_faces  # noqa: E402
import eaf  # noqa: E402

ORIGINALS_DIR = os.environ.get("ANAN_FACES_ORIGINALS", "")

BACKGROUND = (253, 229, 219)
FRAME_GREY = (172, 172, 172)


def solid_color(index):
    return ((index * 37) % 256, (index * 91 + 40) % 256, (index * 53 + 80) % 256)


def write_fake_faces(directory, striped=False, skip=()):
    """Write 21 fake 640x480 sources; striped ones change when shifted."""
    os.makedirs(directory, exist_ok=True)
    for i, name in enumerate(build_faces.FACE_NAMES):
        if name in skip:
            continue
        im = Image.new("RGB", (640, 480), solid_color(i))
        if striped:
            draw = ImageDraw.Draw(im)
            for y in range(0, 480, 16):
                draw.rectangle((0, y, 639, y + 7), fill=solid_color(i + 50))
        im.save(os.path.join(directory, name + ".png"))


def framed_image(size, frame_fraction):
    """A background image with a grey rounded frame of the given thickness."""
    w, h = size
    im = Image.new("RGB", size, (255, 255, 255))
    draw = ImageDraw.Draw(im)
    radius = round(min(w, h) * 0.06)
    draw.rounded_rectangle((0, 0, w - 1, h - 1), radius=radius, fill=FRAME_GREY)
    t = round(min(w, h) * frame_fraction)
    draw.rounded_rectangle((t, t, w - 1 - t, h - 1 - t), radius=radius, fill=BACKGROUND)
    draw.ellipse((w * 0.35, h * 0.3, w * 0.65, h * 0.9), fill=(120, 120, 120))
    return im


def unique_image_frames(path):
    _, frames = eaf.read(path)
    images = frames[:-1]
    return images, len(set(images))


class QuantizeTest(unittest.TestCase):
    def test_black_pixel_palette_alpha_is_255(self):
        black = Image.new("RGB", (320, 240), (0, 0, 0))
        palette, pixels = build_faces.quantize_faces([black])
        self.assertEqual(len(palette), 1024)
        index = pixels[0][0]
        self.assertEqual(palette[index * 4:index * 4 + 4], bytes([0, 0, 0, 255]))
        self.assertTrue(all(palette[i * 4 + 3] == 255 for i in range(256)))


class BuildTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.src = os.path.join(self.tmp.name, "src")
        self.board = os.path.join(self.tmp.name, "board")

    def tearDown(self):
        self.tmp.cleanup()

    def eaf_path(self, name):
        return os.path.join(self.board, "emoji", name)

    def test_build_creates_all_files(self):
        write_fake_faces(self.src)
        build_faces.build(self.src, self.board)
        eafs = sorted(os.listdir(os.path.join(self.board, "emoji")))
        self.assertEqual(len(eafs), 24)
        with open(os.path.join(self.board, "assets", "320_240", "emote.json")) as f:
            entries = json.load(f)
        self.assertEqual(len(entries), 21 + 1 + 3)
        names = [e["emote"] for e in entries]
        self.assertEqual(names, list(build_faces.FACE_NAMES) + ["idle", "idle_sleepy", "idle_wink", "idle_delicious"])
        for entry in entries:
            self.assertTrue(os.path.isfile(self.eaf_path(entry["src"])), entry["src"])
            self.assertLessEqual(len(entry["src"]), 31)
            self.assertEqual(entry["fps"], 5)
        loops = {e["emote"]: e["loop"] for e in entries}
        self.assertTrue(loops["neutral"] and loops["idle"] and loops["happy"])
        self.assertFalse(loops["idle_sleepy"] or loops["idle_wink"] or loops["idle_delicious"])
        self.assertEqual(entries[0], {"emote": "neutral", "src": "anan_neutral.eaf", "loop": True, "fps": 5})
        self.assertEqual(entries[21]["src"], "anan_neutral.eaf")

    def test_emote_json_has_one_entry_per_line(self):
        write_fake_faces(self.src)
        build_faces.build(self.src, self.board)
        with open(os.path.join(self.board, "assets", "320_240", "emote.json")) as f:
            lines = f.read().splitlines()
        self.assertEqual(lines[0], "[")
        self.assertEqual(lines[-1], "]")
        self.assertEqual(lines[2], '    {"emote": "happy", "src": "anan_happy.eaf", "loop": true, "fps": 5},')
        self.assertEqual(len(lines), 25 + 2)

    def test_build_is_deterministic(self):
        write_fake_faces(self.src, striped=True)
        other = os.path.join(self.tmp.name, "board2")
        build_faces.build(self.src, self.board)
        build_faces.build(self.src, other)
        files = []
        for root, _, names in os.walk(self.board):
            files += [os.path.relpath(os.path.join(root, n), self.board) for n in names]
        self.assertEqual(len(files), 26)
        for rel in sorted(files):
            with self.subTest(file=rel):
                self.assertTrue(filecmp.cmp(os.path.join(self.board, rel), os.path.join(other, rel), shallow=False))

    def test_missing_face_fails_with_name(self):
        write_fake_faces(self.src, skip=("kissy",))
        with self.assertRaises(build_faces.FaceError) as ctx:
            build_faces.build(self.src, self.board)
        self.assertIn("kissy", str(ctx.exception))

    def test_wrong_source_size_fails(self):
        write_fake_faces(self.src)
        Image.new("RGB", (320, 240)).save(os.path.join(self.src, "cool.png"))
        with self.assertRaises(build_faces.FaceError) as ctx:
            build_faces.build(self.src, self.board)
        self.assertIn("cool", str(ctx.exception))

    def test_neutral_sequence_is_30_frames_with_2_unique(self):
        write_fake_faces(self.src)
        build_faces.build(self.src, self.board)
        frames, unique = unique_image_frames(self.eaf_path("anan_neutral.eaf"))
        self.assertEqual(len(frames), 30)
        self.assertEqual(unique, 2)
        closed = [i for i, f in enumerate(frames) if f != frames[0]]
        self.assertEqual(closed, [18, 29])

    def test_emotion_sequence_is_20_frames_with_3_unique(self):
        write_fake_faces(self.src, striped=True)
        build_faces.build(self.src, self.board)
        frames, unique = unique_image_frames(self.eaf_path("anan_happy.eaf"))
        self.assertEqual(len(frames), 20)
        self.assertEqual(unique, 3)
        order = []
        for f in frames:
            if f not in order:
                order.append(f)
        pattern = [order.index(f) for f in frames]
        self.assertEqual(pattern, [0] * 8 + [1] * 2 + [2] * 8 + [1] * 2)

    def test_shifted_frame_moves_rows_down_and_repeats_row_0(self):
        write_fake_faces(self.src, striped=True)
        build_faces.build(self.src, self.board)
        _, frames = eaf.read(self.eaf_path("anan_sad.eaf"))
        base = eaf.parse_frame(frames[0])["pixels"]
        shifted = eaf.parse_frame(frames[10])["pixels"]
        w = 320
        self.assertEqual(shifted[:w], base[:w])
        self.assertEqual(shifted[w:2 * w], base[:w])
        self.assertEqual(shifted[2 * w:], base[:-2 * w])

    def test_idle_inc_lines(self):
        write_fake_faces(self.src)
        build_faces.build(self.src, self.board)
        with open(os.path.join(self.board, "anan_idle_actions.inc")) as f:
            lines = [line for line in f.read().splitlines() if line.startswith("ANAN_IDLE_ACTION")]
        self.assertEqual(lines, [
            'ANAN_IDLE_ACTION("idle_sleepy", 3200)',
            'ANAN_IDLE_ACTION("idle_wink", 1800)',
            'ANAN_IDLE_ACTION("idle_delicious", 2400)',
        ])
        frames, _ = unique_image_frames(self.eaf_path("anan_idle_sleepy.eaf"))
        self.assertEqual(len(frames), 16)

    def test_build_removes_stale_generated_files(self):
        write_fake_faces(self.src)
        os.makedirs(os.path.join(self.board, "emoji"))
        stale = self.eaf_path("anan_idle_old.eaf")
        open(stale, "wb").close()
        build_faces.build(self.src, self.board)
        self.assertFalse(os.path.exists(stale))


class PrepareTest(unittest.TestCase):
    def test_prepare_accepts_framed_image_and_outputs_640x480(self):
        out = build_faces.prepare_image(framed_image((1792, 1344), 0.05), "x")
        self.assertEqual(out.size, (640, 480))
        self.assertEqual(out.mode, "RGB")
        self.assertEqual(out.getpixel((0, 0)), BACKGROUND)

    def test_prepare_rejects_non_4_3(self):
        with self.assertRaises(build_faces.FaceError) as ctx:
            build_faces.prepare_image(framed_image((1920, 1080), 0.05), "wide")
        self.assertIn("wide", str(ctx.exception))
        # 1824x1376 is 0.6% off 4:3: accepted and centre-cropped.
        out = build_faces.prepare_image(framed_image((1824, 1376), 0.05), "near")
        self.assertEqual(out.size, (640, 480))

    def test_prepare_rejects_border_left_in_corners(self):
        with self.assertRaises(build_faces.FaceError) as ctx:
            build_faces.prepare_image(framed_image((1792, 1344), 0.12), "thick")
        self.assertIn("thick", str(ctx.exception))
        self.assertIn("frame", str(ctx.exception))

    def test_prepare_writes_all_faces_and_fails_on_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, out = os.path.join(tmp, "src"), os.path.join(tmp, "out")
            os.makedirs(src)
            for name in build_faces.FACE_NAMES:
                framed_image((800, 600), 0.04).save(os.path.join(src, name + ".png"))
            build_faces.prepare(src, out)
            self.assertEqual(sorted(os.listdir(out)), sorted(n + ".png" for n in build_faces.FACE_NAMES))
            os.remove(os.path.join(src, "silly.png"))
            with self.assertRaises(build_faces.FaceError) as ctx:
                build_faces.prepare(src, out)
            self.assertIn("silly", str(ctx.exception))


@unittest.skipUnless(ORIGINALS_DIR and os.path.isdir(ORIGINALS_DIR), "set ANAN_FACES_ORIGINALS to check the real originals")
class RealOriginalsTest(unittest.TestCase):
    """Optional: the border check passes on every real original with the 8% inset."""

    def test_border_check_passes_on_all_originals(self):
        for name in build_faces.FACE_NAMES:
            with self.subTest(face=name):
                with Image.open(os.path.join(ORIGINALS_DIR, name + ".png")) as im:
                    build_faces.prepare_image(im, name)

    def test_border_check_fails_without_inset(self):
        for name in build_faces.FACE_NAMES:
            with self.subTest(face=name):
                with Image.open(os.path.join(ORIGINALS_DIR, name + ".png")) as im:
                    with self.assertRaises(build_faces.FaceError):
                        build_faces.prepare_image(im, name, inset=0.0)


if __name__ == "__main__":
    unittest.main()
