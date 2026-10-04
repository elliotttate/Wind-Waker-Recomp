#!/usr/bin/env python3
"""Decode a GameCube/Wii CISO (compact ISO) back to a raw disc image.

Format (Dolphin Source/Core/DiscIO/CISOBlob.h):
  offset 0x00  u32  magic "CISO" (bytes 43 49 53 4F)
  offset 0x04  u32  block_size (little endian)
  offset 0x08  u8   map[0x7FF8]  0=unused (sparse, all zero), 1=used
  offset 0x8000      data blocks, block_size bytes each, only for used blocks

A GameCube disc is a fixed 1,459,978,240 bytes (0x57058000). Unused blocks are
emitted as zeroes so the output is a plain, uncompressed .iso that BlueWake's
disc_extract / bluewake_disc_check accepts.

Usage: ciso2iso.py INPUT.ciso OUTPUT.iso
"""
import struct
import sys

GC_DISC_SIZE = 0x57058000  # 1,459,978,240 bytes, standard GameCube disc

def fail(msg):
    print(f"ciso2iso: {msg}", file=sys.stderr)
    sys.exit(1)

def main():
    if len(sys.argv) != 3:
        fail("usage: ciso2iso.py INPUT.ciso OUTPUT.iso")
    src, dst = sys.argv[1], sys.argv[2]

    with open(src, "rb") as f:
        hdr = f.read(0x8000)
        if len(hdr) < 0x8000:
            fail("file too small to be a CISO")
        if hdr[:4] != b"CISO":
            fail("not a CISO image (missing CISO magic)")
        (block_size,) = struct.unpack("<I", hdr[4:8])
        if block_size == 0 or block_size & (block_size - 1):
            fail(f"implausible block_size {block_size}")
        # map: bytes 0x08 .. 0x8000, one u8 per block
        raw_map = hdr[8:0x8000]

        nblocks = (GC_DISC_SIZE + block_size - 1) // block_size
        if nblocks > len(raw_map):
            fail(f"map too small: need {nblocks} entries, have {len(raw_map)}")

        used = [b == 1 for b in raw_map[:nblocks]]
        n_used = sum(used)
        print(f"block_size={block_size} blocks={nblocks} used={n_used} "
              f"(sparse={nblocks - n_used})")

        with open(dst, "wb") as out:
            written = 0
            for i in range(nblocks):
                remaining = GC_DISC_SIZE - written
                take = min(block_size, remaining)
                if used[i]:
                    chunk = f.read(block_size)
                    if len(chunk) < take:
                        fail(f"truncated data block {i}")
                    out.write(chunk[:take])
                else:
                    out.write(b"\x00" * take)
                written += take
        print(f"wrote {dst} ({written} bytes)")

if __name__ == "__main__":
    main()
