"""Writes package/RelentlessPursuit.esp: an ESL-flagged plugin with no masters and one record,
an encounter zone with 'Disable Combat Boundary' set (FormID 0x800). The DLL gives this zone to
cells that have none while the player is in them, so enemies can follow the player into them.

Run: python tools/make_esp.py
"""
import pathlib
import struct


def sub(sig: bytes, data: bytes) -> bytes:
    return sig + struct.pack("<H", len(data)) + data


def record(sig: bytes, form_id: int, data: bytes, flags: int = 0) -> bytes:
    # signature, data size, flags, FormID, version control info, form version 44, unknown
    return sig + struct.pack("<IIIIHH", len(data), flags, form_id, 0, 44, 0) + data


def group(label: bytes, contents: bytes) -> bytes:
    return b"GRUP" + struct.pack("<I", 24 + len(contents)) + label + struct.pack("<IIHH", 0, 0, 0, 0) + contents


ZONE_ID = 0x800
DISABLE_COMBAT_BOUNDARY = 0x04

header = (
    sub(b"HEDR", struct.pack("<fII", 1.7, 1, ZONE_ID + 1))
    + sub(b"CNAM", b"Paraskevas\0")
    + sub(b"SNAM", b"Relentless Pursuit: a no-boundary encounter zone for cells that have none.\0")
    + sub(b"INTV", struct.pack("<I", 1))
)
tes4 = record(b"TES4", 0, header, flags=0x200)  # 0x200 = light plugin (ESL)

zone = (
    sub(b"EDID", b"RP_NoBoundaryZone\0")
    # owner, location, owner rank, min level, flags, max level (0 = no limit)
    + sub(b"DATA", struct.pack("<IIbbBb", 0, 0, 0, 0, DISABLE_COMBAT_BOUNDARY, 0))
)
plugin = tes4 + group(b"ECZN", record(b"ECZN", ZONE_ID, zone))

out = pathlib.Path(__file__).resolve().parent.parent / "package" / "RelentlessPursuit.esp"
out.write_bytes(plugin)
print(f"wrote {out} ({len(plugin)} bytes)")
