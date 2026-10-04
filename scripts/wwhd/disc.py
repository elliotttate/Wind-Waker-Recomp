"""Random access to user-supplied WUD/WUX discs, without expanding the image.

Partition/FST and hashed AES layouts follow Maschell's JNUSLib (GPL-3.0-or-
later), https://github.com/Maschell/JNUSLib. Only local key files are read;
keys are never printed, downloaded, or copied into output manifests.
"""
from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache
import hashlib
from pathlib import Path
import struct

from Crypto.Cipher import AES

from .formats import FormatError, MAX_FILE, Reader, safe_name


def read_key(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) != 16:
        try:
            data = bytes.fromhex(data.decode().strip())
        except (ValueError, UnicodeError):
            raise FormatError("key files must contain 16 binary bytes or 32 hex digits") from None
    if len(data) != 16:
        raise FormatError("key files must contain 16 binary bytes or 32 hex digits")
    return data


class DiscImage:
    def __init__(self, path: Path):
        self.file = path.open("rb")
        self.physical_size = path.stat().st_size
        h = self.file.read(32)
        self.table = None
        self.size = self.physical_size
        if h[:4] == b"WUX0":
            if h[4:8] != b"\x2e\xd0\x99\x10":
                raise FormatError("invalid WUX header")
            self.sector, flags, self.size = struct.unpack_from("<IIQ", h, 8)
            if flags or self.sector != 0x8000 or self.size > 64 * 1024**3:
                raise FormatError("unsupported WUX layout")
            n = (self.size + self.sector - 1) // self.sector
            table = self.file.read(n * 4)
            if len(table) != n * 4:
                raise FormatError("truncated WUX sector map")
            self.table = struct.unpack(f"<{n}I", table)
            self.base = ((32 + n * 4 + self.sector - 1) // self.sector) * self.sector
            required = self.base + (max(self.table) + 1) * self.sector
            if required > self.physical_size:
                raise FormatError(f"WUX is incomplete: expected {required} bytes, found {self.physical_size}; re-extract or re-dump it")

    def close(self):
        self.file.close()

    def read(self, at: int, size: int) -> bytes:
        if at < 0 or size < 0 or size > MAX_FILE or at + size > self.size:
            raise FormatError("disc read outside image")
        result = bytearray()
        while size:
            if self.table is not None:
                index, within = divmod(at, self.sector)
                physical = self.base + self.table[index] * self.sector + within
                n = min(size, self.sector - within)
            else:
                physical, n = at, size
            self.file.seek(physical)
            chunk = self.file.read(n)
            if len(chunk) != n:
                raise FormatError("disc data is missing or truncated")
            result.extend(chunk)
            at += n
            size -= n
        return bytes(result)


@dataclass
class Entry:
    path: str
    offset: int
    size: int
    cluster: int
    flags: int


def fst(data: bytes):
    r = Reader(data)
    if r.take(0, 4) != b"FST\0":
        raise FormatError("incorrect title key or invalid Wii U file table")
    scale, n = r.u32(4), r.u32(8)
    clusters = [r.unpack("2I", 32 + i * 32) for i in range(n)]
    base = 32 + n * 32
    count = r.u32(base + 8)
    strings = base + count * 16
    r.take(base, count * 16)
    entries = []
    def walk(start, end, prefix, depth):
        if depth > 32 or end > count:
            raise FormatError("invalid Wii U directory tree")
        i = start
        while i < end:
            typ_name, off, size, flags, cluster = r.unpack("3I2H", base + i * 16)
            name = safe_name(prefix + r.string(strings + (typ_name & 0xFFFFFF)))
            if typ_name >> 24 & 1:
                if not i < size <= end:
                    raise FormatError("invalid Wii U directory range")
                walk(i + 1, size, name + "/", depth + 1)
                i = size
            else:
                if cluster >= n:
                    raise FormatError("invalid Wii U content index")
                entries.append(Entry(name, off * scale, size, cluster, flags))
                i += 1
    walk(1, count, "", 0)
    return clusters, entries


class WiiUDisc:
    def __init__(self, path: Path, disc_key: bytes, title_key: bytes | None = None,
                 common_key: bytes | None = None):
        self.image = DiscImage(path)
        self.disc_key, self.title_key = disc_key, title_key
        self.identifier = self.image.read(0, 64).split(b"\0")[0].decode("ascii")
        if not self.identifier.startswith("WUP-P-BCZE-"):
            raise FormatError("expected Wind Waker HD USA disc (BCZE)")
        toc = Reader(self.metadata(0x18000, 0x8000, disc_key))
        if toc.take(0, 4) != b"\xcc\xa6\xe6\x7b":
            raise FormatError("incorrect WUD disc key")
        self.partitions = {}
        for i in range(toc.u32(28)):
            at = 0x800 + i * 0x80
            self.partitions[toc.string(at)] = toc.u32(at + 32) * 0x8000
        gm = [k for k in self.partitions if k.startswith("GM0005000010143500")]
        if len(gm) != 1 or "SI" not in self.partitions:
            raise FormatError("USA Wind Waker HD game partition not found")
        si, si_size = self.partition_header(self.partitions["SI"])
        clusters, entries = fst(self.metadata(self.partitions["SI"] + si, si_size, disc_key))
        tmds = [e for e in entries if e.path.lower().endswith("/title.tmd")]
        self.contents = None
        for e in tmds:
            start = self.partitions["SI"] + si + max(0, clusters[e.cluster][0] - 1) * 0x8000
            raw = self.system_content(start, e.offset, e.size)
            r = Reader(raw)
            if r.take(0x18C, 8) != bytes.fromhex("0005000010143500"):
                continue
            if title_key is None:
                if common_key is None:
                    raise FormatError("provide the disc title key, or a common key to decrypt its ticket")
                ticket_entry = next(t for t in entries if t.path == e.path.removesuffix(".tmd") + ".tik")
                ticket_start = self.partitions["SI"] + si + max(0, clusters[ticket_entry.cluster][0] - 1) * 0x8000
                ticket = Reader(self.system_content(ticket_start, ticket_entry.offset, ticket_entry.size))
                title_key = AES.new(common_key, AES.MODE_CBC, ticket.take(0x1DC, 8) + bytes(8)).decrypt(ticket.take(0x1BF, 16))
                self.title_key = title_key
            self.contents = {}
            for i in range(r.u16(0x1DE)):
                _, index, typ, size = r.unpack("IHHQ", 0xB04 + i * 48)
                self.contents[index] = (typ, size, r.take(0xB14 + i * 48, 20))
        if self.contents is None:
            raise FormatError("Wind Waker HD title metadata not found")
        header, size = self.partition_header(self.partitions[gm[0]])
        self.start = self.partitions[gm[0]] + header
        _, fst_size, digest = self.contents[0]
        raw = self.metadata(self.start, fst_size, title_key)
        if hashlib.sha1(raw[:fst_size]).digest() != digest:
            raise FormatError("title key or game file table checksum is incorrect")
        self.clusters, self.entries = fst(raw[:size])

    def close(self):
        self.hashed_block.cache_clear()
        self.image.close()

    def metadata(self, at, size, key):
        # Metadata/FST uses CBC with a fixed zero IV. It fits one 64 KiB
        # block for the supported disc; reset at every block for system data.
        return AES.new(key, AES.MODE_CBC, bytes(16)).decrypt(self.image.read(at, (size + 15) // 16 * 16))[:size]

    def partition_header(self, at):
        r = Reader(self.image.read(at, 32))
        if r.take(0, 4) != b"\xcc\x93\xa4\xf5":
            raise FormatError("invalid Wii U partition header")
        return r.u32(4), r.u32(20)

    def system_content(self, start, at, size):
        result = bytearray()
        while size:
            index, off = divmod(at, 0x10000)
            raw = self.image.read(start + index * 0x10000, 0x10000)
            iv = bytes(8) + index.to_bytes(8, "big")
            plain = AES.new(self.disc_key, AES.MODE_CBC, iv).decrypt(raw)
            n = min(size, 0x10000 - off)
            result.extend(plain[off:off + n])
            at += n
            size -= n
        return bytes(result)

    @lru_cache(maxsize=32)
    def hashed_block(self, cluster, index):
        start = self.start + max(0, self.clusters[cluster][0] - 1) * 0x8000
        raw = self.image.read(start + index * 0x10000, 0x10000)
        hashes = AES.new(self.title_key, AES.MODE_CBC, bytes(16)).decrypt(raw[:0x400])
        h0 = hashes[index % 16 * 20:index % 16 * 20 + 20]
        plain = AES.new(self.title_key, AES.MODE_CBC, h0[:16]).decrypt(raw[0x400:])
        if hashlib.sha1(plain).digest() != h0:
            raise FormatError("Wii U content block checksum failed")
        return plain

    def read(self, entry: Entry):
        if entry.size > MAX_FILE:
            raise FormatError("disc file is too large")
        typ, _, _ = self.contents[entry.cluster]
        start = self.start + max(0, self.clusters[entry.cluster][0] - 1) * 0x8000
        at, size = entry.offset, entry.size
        if typ & 2:
            out = bytearray()
            while size:
                block, off = divmod(at, 0xFC00)
                plain = self.hashed_block(entry.cluster, block)
                n = min(size, 0xFC00 - off)
                out.extend(plain[off:off + n])
                at += n
                size -= n
            return bytes(out)
        if typ & 1:
            aligned = at // 16 * 16
            iv = (self.image.read(start + aligned - 16, 16) if aligned else
                  entry.cluster.to_bytes(2, "big") + bytes(14))
            length = (at - aligned + size + 15) // 16 * 16
            raw = self.image.read(start + aligned, length)
            return AES.new(self.title_key, AES.MODE_CBC, iv).decrypt(raw)[at - aligned:at - aligned + size]
        return self.image.read(start + at, size)
