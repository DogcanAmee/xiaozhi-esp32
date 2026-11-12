"""Reader and writer for Espressif emote animation (``.eaf``) files.

Only the subset the Anan faces need is written: 8-bit palette frames whose
pixel blocks are run-length encoded.  The reader also understands raw
(uncompressed) blocks so it can inspect other files.

Container layout (all integers little-endian)::

    0x89 "EAF"                      4 bytes
    frame count                     u32
    checksum                        u32  32-bit byte sum of everything after byte 16
    length                          u32  file length - 16
    frame table                     count x (u32 size, u32 offset); offset is
                                         relative to the end of the table
    frame data

``_S`` (image) frame::

    0x5A 0x5A "_S"
    7 header bytes                  00 00 00 00 00 00 01
    bit depth                       u8   (8)
    width, height, blocks, block_h  u16 x 4
    block lengths                   blocks x u32
    palette                         256 x (B, G, R, A)
    blocks                          first byte = encoding; 0 = RLE as
                                    (count 1-255, value) pairs, 5 = raw bytes

The player treats the last table entry as a ``_C`` (comment) trailer and plays
(entries - 1) frames, so :func:`write` always appends one.
"""

import struct

FRAME_MAGIC = b"\x5a\x5a"
CONTAINER_TAGS = (b"EAF", b"AAF")
CONTAINER_HEADER_SIZE = 16
TABLE_ENTRY_SIZE = 8

IMAGE_FRAME = b"_S"
COMMENT_FRAME = b"_C"
IMAGE_HEADER = b"\x00\x00\x00\x00\x00\x00\x01"
BIT_DEPTH = 8
PALETTE_SIZE = 256 * 4

ENCODING_RLE = 0
ENCODING_RAW = 5
MAX_RUN = 255

# Same 14 bytes as the stock trailers, then a u32 text length and the text.
COMMENT_HEADER = b"\x5a\x5a_C\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00"
TRAILER_TEXT = b"anan faces 1"


def checksum(data):
    """Return the container checksum: the 32-bit sum of all bytes."""
    return sum(data) & 0xFFFFFFFF


def read(path):
    """Read a container and return ``(tag, frames)``.

    ``frames`` holds the raw bytes of every table entry, trailer included.
    Raises ``ValueError`` if the magic, length or checksum is wrong.
    """
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < CONTAINER_HEADER_SIZE or data[0] != 0x89 or data[1:4] not in CONTAINER_TAGS:
        raise ValueError("%s: not an EAF container" % path)
    count, stored_sum, length = struct.unpack("<III", data[4:16])
    if length != len(data) - CONTAINER_HEADER_SIZE:
        raise ValueError("%s: length field %d, expected %d" % (path, length, len(data) - 16))
    if checksum(data[CONTAINER_HEADER_SIZE:]) != stored_sum:
        raise ValueError("%s: bad checksum" % path)
    base = CONTAINER_HEADER_SIZE + count * TABLE_ENTRY_SIZE
    frames = []
    for i in range(count):
        pos = CONTAINER_HEADER_SIZE + i * TABLE_ENTRY_SIZE
        size, offset = struct.unpack("<II", data[pos:pos + TABLE_ENTRY_SIZE])
        frames.append(data[base + offset:base + offset + size])
    return data[1:4], frames


def parse_frame(frame):
    """Decode one frame into a dict.

    ``_S`` frames give ``kind, w, h, bit_depth, block_height, blocks, palette,
    pixels`` (one palette index per byte).  Any other kind gives ``kind`` and
    ``raw``; a ``_C`` trailer also gives ``text``.
    """
    if frame[:2] != FRAME_MAGIC:
        raise ValueError("frame does not start with 0x5A5A")
    kind = frame[2:4]
    if kind == COMMENT_FRAME:
        (text_len,) = struct.unpack("<I", frame[14:18])
        return {"kind": kind, "raw": frame, "text": frame[18:18 + text_len]}
    if kind != IMAGE_FRAME:
        return {"kind": kind, "raw": frame}
    bit_depth = frame[11]
    if bit_depth != BIT_DEPTH:
        raise ValueError("only 8-bit frames are supported, got %d" % bit_depth)
    w, h, blocks, block_height = struct.unpack("<HHHH", frame[12:20])
    pos = 20
    lengths = struct.unpack("<%dI" % blocks, frame[pos:pos + 4 * blocks])
    pos += 4 * blocks
    palette = frame[pos:pos + PALETTE_SIZE]
    pos += PALETTE_SIZE
    pixels = bytearray()
    for length in lengths:
        block = frame[pos:pos + length]
        pos += length
        if block[0] == ENCODING_RLE:
            body = block[1:]
            for k in range(0, len(body) - 1, 2):
                pixels += bytes([body[k + 1]]) * body[k]
        elif block[0] == ENCODING_RAW:
            pixels += block[1:]
        else:
            raise ValueError("block encoding %d is not supported" % block[0])
    if len(pixels) != w * h:
        raise ValueError("decoded %d pixels, expected %d" % (len(pixels), w * h))
    return {"kind": kind, "w": w, "h": h, "bit_depth": bit_depth,
            "block_height": block_height, "blocks": blocks,
            "palette": bytes(palette), "pixels": bytes(pixels)}


def rle_encode(data):
    """Encode bytes as (count, value) pairs with counts of 1-255."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        value = data[i]
        j = i + 1
        while j < n and data[j] == value and j - i < MAX_RUN:
            j += 1
        out += bytes([j - i, value])
        i = j
    return bytes(out)


def build_frame(w, h, palette, pixels, block_height=32):
    """Build an 8-bit ``_S`` frame with RLE blocks of ``block_height`` rows.

    ``palette`` is 256 BGRA entries (1024 bytes); ``pixels`` is ``w * h``
    palette indices, row by row.
    """
    if len(palette) != PALETTE_SIZE:
        raise ValueError("palette must be %d bytes, got %d" % (PALETTE_SIZE, len(palette)))
    if len(pixels) != w * h:
        raise ValueError("pixels must be %d bytes, got %d" % (w * h, len(pixels)))
    blocks = (h + block_height - 1) // block_height
    bodies = []
    for b in range(blocks):
        rows = pixels[b * block_height * w:min((b + 1) * block_height, h) * w]
        bodies.append(bytes([ENCODING_RLE]) + rle_encode(rows))
    return (FRAME_MAGIC + IMAGE_FRAME + IMAGE_HEADER + bytes([BIT_DEPTH])
            + struct.pack("<HHHH", w, h, blocks, block_height)
            + struct.pack("<%dI" % blocks, *[len(body) for body in bodies])
            + bytes(palette) + b"".join(bodies))


def build_trailer(text=TRAILER_TEXT):
    """Build the ``_C`` trailer frame carrying ``text``."""
    return COMMENT_HEADER + struct.pack("<I", len(text)) + text


def write(path, frames):
    """Write ``frames`` plus a ``_C`` trailer to ``path``; return the file size.

    Identical frames are stored once and their table entries share an offset.
    """
    entries = list(frames) + [build_trailer()]
    table, blob, offsets = [], bytearray(), {}
    for frame in entries:
        if frame not in offsets:
            offsets[frame] = len(blob)
            blob += frame
        table.append(struct.pack("<II", len(frame), offsets[frame]))
    body = b"".join(table) + bytes(blob)
    data = b"\x89EAF" + struct.pack("<III", len(entries), checksum(body), len(body)) + body
    with open(path, "wb") as f:
        f.write(data)
    return len(data)
