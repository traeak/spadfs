# Crash-count table integrity

Primed volume: tx.cc=4, cct[cc]=1776, CC_VALID=True

| Class | cct[cc] after | delta | CC_VALID | fsck | Verdict |
|---|---|---|---|---|---|
| bitflip | 1777 | +1 | True | rc=0 | **SILENT** |
| byte_transpose | 61446 | +59670 | True | rc=0 | **SILENT** |
| word_swap | 0 | -1776 | False | rc=4 | detected |
| word_dup | 1776 | +0 | True | rc=4 | detected |
| xor_cancel | 1777 | +1 | True | rc=4 | detected |
| zero_out | 0 | -1776 | False | rc=0 | **SILENT** |

## Summary

- live entry corrupted without any fsck complaint: **3**
- of those, CC_VALID flipped: **1**

The detections that *do* happen come from the ordering rule in
sckcct.c (entries above tx.cc must be zero), not from a checksum --
which is why classes that spill above cc are caught and classes confined
to cct[cc] itself are not.
