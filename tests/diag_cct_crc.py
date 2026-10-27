#!/usr/bin/env python3
"""Read-only forensic dump of the cct side-CRC region.

The kernel mount now succeeds, but fsck after real writes reports:

    CCT CRC UNIT 1 CHECKSUM MISMATCH
    CCT CRC UNIT 2 CHECKSUM MISMATCH
    CCT CRC UNIT 3 CHECKSUM MISMATCH
    CCT CRC UNIT 4 CHECKSUM MISMATCH
    CCT CRC TABLE UNTRUSTWORTHY (4 BAD UNIT(S)); CANNOT VERIFY CCT

Unit 0 is fine. That distinction matters: "UNIT CHECKSUM MISMATCH" is the
512-byte unit's own trailer failing spadfs_meta_verify -- not a sector CRC
disagreement. So either units 1..4 were rewritten with a bad seal, or they
were never sealed correctly on this device.

This script separates the two. It answers, per unit:

  A. does the unit's trailer match crc32c(unit[0:508])?   (the seal)
  B. does each slot's stored CRC match crc32c of the cct sector it covers?
     (the content, independent of the seal)

and per cct sector:

  C. is the sector still all-zero, i.e. did the kernel touch it at all?

Run as root, read-only. Does not modify the device.

  sudo python3 tests/diag_cct_crc.py /dev/sda1
"""

import os  # noqa: E402
import sys  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sbinfo import SECTOR, read_super  # noqa: E402
from prime import META_COVER, crc32c_std  # noqa: E402

CCT_SECTORS = 512
CRC_PER_UNIT = META_COVER // 4                # 127
UNITS = -(-CCT_SECTORS // CRC_PER_UNIT)      # 5


def main():
    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/sda1"
    f = open(dev, "rb")  # noqa: SIM115 - long-lived handle, closed at end

    info = read_super(dev)
    cct = info["cct"]
    crc = info["cct_crc"]
    bs = 1 << info["sectors_per_block_bits"]
    print(f"device        : {dev}")
    print(f"cct sector    : {cct}")
    print(f"cct_crc sector: {crc}   (block_sectors={bs})")
    print()

    # ---- C: which cct sectors did the kernel actually modify? ----
    nonzero = []
    for s in range(CCT_SECTORS):
        f.seek((cct + s) * SECTOR)
        d = f.read(SECTOR)
        if any(d):
            nonzero.append(s)
    print(f"cct sectors that are NOT all-zero: {len(nonzero)}")
    if nonzero:
        print(f"  first few: {nonzero[:12]}")
        print(f"  last few : {nonzero[-6:]}")
    print()

    # ---- A: unit seals ----
    print("UNIT SEAL  (trailer at 508 vs crc32c(unit[0:508]))")
    units = []
    for u in range(UNITS):
        f.seek((crc + u) * SECTOR)
        unit = f.read(SECTOR)
        units.append(unit)
        stored = int.from_bytes(unit[508:512], "little")
        comp = crc32c_std(unit[:META_COVER]) & 0xFFFFFFFF
        allzero = not any(unit)
        print(f"  unit {u}: stored {stored:08x} computed {comp:08x} "
              f"{'MATCH  ' if stored == comp else 'MISMATCH'}"
              f"{'   <-- region is ALL ZERO (never written)' if allzero else ''}")
    print()

    # ---- B: content, only for units whose seal passed ----
    print("SLOT CONTENT  (stored CRC vs crc32c of the covered cct sector)")
    for u in range(UNITS):
        unit = units[u]
        stored = int.from_bytes(unit[508:512], "little")
        if stored != (crc32c_std(unit[:META_COVER]) & 0xFFFFFFFF):
            print(f"  unit {u}: seal bad, slots unreadable-as-trustworthy -- skipped")
            continue
        bad = 0
        first_bad = None
        for i in range(CRC_PER_UNIT):
            sec = u * CRC_PER_UNIT + i
            if sec >= CCT_SECTORS:
                break
            sv = int.from_bytes(unit[i * 4:i * 4 + 4], "little")
            f.seek((cct + sec) * SECTOR)
            cv = crc32c_std(f.read(SECTOR)) & 0xFFFFFFFF
            if sv != cv:
                bad += 1
                if first_bad is None:
                    first_bad = (sec, sv, cv)
        print(f"  unit {u}: {bad} bad slot(s)" +
              ("" if first_bad is None else
               f"  e.g. cct sector {first_bad[0]} stored {first_bad[1]:08x} "
               f"actual {first_bad[2]:08x}"))
    print()

    # ---- what the backup mirror holds, for comparison ----
    if info.get("cct_backup"):
        b, bc = info["cct_backup"], info["cct_backup_crc"]
        print(f"BACKUP mirror at {b}, its CRC region at {bc}")
        f.seek(b * SECTOR)
        bd = f.read(SECTOR)
        print(f"  backup sector 0 all-zero: {not any(bd)}")
        f.seek(bc * SECTOR)
        bu = f.read(SECTOR)
        s = int.from_bytes(bu[508:512], "little")
        c = crc32c_std(bu[:META_COVER]) & 0xFFFFFFFF
        print(f"  backup crc unit 0 seal  : stored {s:08x} computed {c:08x} "
              f"{'MATCH' if s == c else 'MISMATCH'}")
        print("  (the backup is mkfs-only until Task 3.3, so it still describes")
        print("   the all-zero table -- which is why fsck says it 'verifies' while")
        print("   being completely stale.)")

    f.close()


if __name__ == "__main__":
    main()
