"""Tests for the .eaf reader/writer."""

import glob
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import eaf  # noqa: E402

STOCK_DIR = os.environ.get("ANAN_STOCK_EAF_DIR", "/tmp/emote/esp_emote_assets/emoji_large")


def make_palette():
    return b"".join(bytes([i, 255 - i, i // 2, 255]) for i in range(256))


class EafTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.tmp.name, "t.eaf")

    def tearDown(self):
        self.tmp.cleanup()

    def test_roundtrip_small_frame(self):
        w, h = 8, 40
        pixels = bytes((x * 7 + y * 3) % 256 for y in range(h) for x in range(w))
        palette = make_palette()
        info = eaf.parse_frame(eaf.build_frame(w, h, palette, pixels))
        self.assertEqual(info["kind"], b"_S")
        self.assertEqual((info["w"], info["h"]), (w, h))
        self.assertEqual(info["pixels"], pixels)
        self.assertEqual(info["palette"], palette)
        self.assertEqual(info["blocks"], 2)
        self.assertEqual(info["block_height"], 32)

    def test_rle_run_longer_than_255_is_split(self):
        pixels = bytes([9]) * 600
        frame = eaf.build_frame(600, 1, make_palette(), pixels)
        self.assertEqual(eaf.parse_frame(frame)["pixels"], pixels)
        self.assertEqual(eaf.rle_encode(pixels), bytes([255, 9, 255, 9, 90, 9]))

    def test_write_dedups_identical_frames_and_appends_trailer(self):
        a = eaf.build_frame(8, 8, make_palette(), bytes(range(64)))
        b = eaf.build_frame(8, 8, make_palette(), bytes(64))
        size = eaf.write(self.path, [a, b, a])
        self.assertEqual(size, os.path.getsize(self.path))
        tag, frames = eaf.read(self.path)
        self.assertEqual(tag, b"EAF")
        self.assertEqual(len(frames), 4)
        self.assertEqual(frames[0], frames[2])
        self.assertEqual(frames[0], a)
        self.assertEqual(frames[1], b)
        self.assertEqual(frames[3][:4].hex(), "5a5a5f43")
        self.assertEqual(eaf.parse_frame(frames[3])["text"], b"anan faces 1")
        trailer = eaf.build_trailer()
        undeduped = 16 + 4 * 8 + 2 * len(a) + len(b) + len(trailer)
        self.assertLess(size, undeduped)
        self.assertEqual(size, undeduped - len(a))

    def test_container_checksum_and_length(self):
        a = eaf.build_frame(8, 8, make_palette(), bytes(range(64)))
        eaf.write(self.path, [a])
        with open(self.path, "rb") as f:
            data = f.read()
        self.assertEqual(data[:4], b"\x89EAF")
        count, stored_sum, length = struct.unpack("<III", data[4:16])
        self.assertEqual(count, 2)
        self.assertEqual(stored_sum, sum(data[16:]) & 0xFFFFFFFF)
        self.assertEqual(length, len(data) - 16)

    def test_read_rejects_corrupted_file(self):
        eaf.write(self.path, [eaf.build_frame(8, 8, make_palette(), bytes(64))])
        with open(self.path, "r+b") as f:
            f.seek(40)
            f.write(b"\x01")
        with self.assertRaises(ValueError):
            eaf.read(self.path)

    def test_trailer_layout(self):
        trailer = eaf.build_trailer()
        self.assertEqual(trailer[:14].hex(), "5a5a5f430000000000000100" "0000")
        self.assertEqual(struct.unpack("<I", trailer[14:18])[0], len(b"anan faces 1"))
        self.assertEqual(trailer[18:], b"anan faces 1")


@unittest.skipUnless(os.path.isdir(STOCK_DIR), "stock .eaf files not present")
class StockFilesTest(unittest.TestCase):
    """Optional: compare against the official asset pack when it is available."""

    def stock_files(self):
        paths = sorted(glob.glob(os.path.join(STOCK_DIR, "*.eaf")))
        self.assertTrue(paths)
        return paths

    def test_reencoding_stock_frames_is_byte_identical(self):
        for path in self.stock_files():
            _, frames = eaf.read(path)
            for i, frame in enumerate(frames):
                info = eaf.parse_frame(frame)
                if info["kind"] != b"_S":
                    continue
                with self.subTest(file=os.path.basename(path), frame=i):
                    rebuilt = eaf.build_frame(info["w"], info["h"], info["palette"],
                                              info["pixels"], info["block_height"])
                    self.assertEqual(rebuilt, frame)

    def test_stock_trailer_has_same_structure(self):
        ours = eaf.build_trailer()
        for path in self.stock_files():
            with self.subTest(file=os.path.basename(path)):
                _, frames = eaf.read(path)
                stock = frames[-1]
                self.assertEqual(stock[:14], ours[:14])
                (text_len,) = struct.unpack("<I", stock[14:18])
                self.assertEqual(len(stock), 18 + text_len)
                self.assertEqual(eaf.parse_frame(stock)["kind"], b"_C")


if __name__ == "__main__":
    unittest.main()
