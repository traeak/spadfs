# Crash-count table integrity

Primed volume: tx.cc=4, cct[cc]=1776, CC_VALID=True

| Class | cct[cc] after | delta | CC_VALID | fsck | Verdict |
|---|---|---|---|---|---|
| bitflip | 1777 | +1 | True | rc=4 | detected |
| byte_transpose | 61446 | +59670 | True | rc=4 | detected |
| word_swap | 0 | -1776 | False | rc=4 | detected |
| word_dup | 1776 | +0 | True | rc=4 | detected |
| xor_cancel | 1777 | +1 | True | rc=4 | detected |
| zero_out | 0 | -1776 | False | rc=4 | detected |

## Summary

- live entry corrupted without any fsck complaint: **0**
- of those, CC_VALID flipped: **0**

All classes are now caught by the cct side CRC table (check_cct_crc, Phase 2.3 A5).
Before it, three of these six were silent -- including zero_out, which
flipped CC_VALID True->False on a volume fsck called clean.
