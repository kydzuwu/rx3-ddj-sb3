"""Read/write Linux newc initramfs archives without creating device nodes on macOS."""
from dataclasses import dataclass


@dataclass
class Entry:
    name: str
    mode: int
    data: bytes = b""
    rdevmajor: int = 0
    rdevminor: int = 0


def read(data):
    entries = {}
    offset = 0
    while offset + 110 <= len(data):
        header = data[offset:offset + 110]
        if header[:6] not in (b"070701", b"070702"):
            raise ValueError(f"Invalid newc header at {offset}")
        fields = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        size, namesize = fields[6], fields[11]
        name = data[offset + 110:offset + 110 + namesize - 1].decode()
        start = (offset + 110 + namesize + 3) & ~3
        if name == "TRAILER!!!":
            break
        entries[name] = Entry(name, fields[1], data[start:start + size], fields[9], fields[10])
        offset = (start + size + 3) & ~3
    return entries


def write(entries):
    out = bytearray()
    for inode, entry in enumerate([*entries, Entry("TRAILER!!!", 0)], 1):
        name = entry.name.encode() + b"\0"
        fields = [inode, entry.mode, 0, 0, 1, 0, len(entry.data), 0, 0,
                  entry.rdevmajor, entry.rdevminor, len(name), 0]
        out.extend(b"070701" + "".join(f"{f:08x}" for f in fields).encode())
        out.extend(name)
        out.extend(b"\0" * (-len(out) % 4))
        out.extend(entry.data)
        out.extend(b"\0" * (-len(out) % 4))
    out.extend(b"\0" * (-len(out) % 512))
    return bytes(out)
