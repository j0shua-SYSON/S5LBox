# Native arithmetic and scalar extra transfers

The signed compact runner now includes ordinary A32 MUL/MLA and
UMULL/UMLAL/SMULL/SMLAL, plus addressing-mode-3 LDRH/STRH/LDRSB/LDRSH.
These are shared instruction semantics, independent of guest programs or PCs.
This is an implementation and correctness candidate, not an established
physical application-speed improvement.

The decoder enters through existing multiply/extra-transfer refusals. Ordinary
DP and Thumb paths have no added common-path decode instructions. Cold PC
checks distinguish a legal extra-transfer literal base or immediate offset
from an unsupported ordinary DP PC operand. Exact multiply masks exclude
synchronization instructions.

Multiply reads all sources before writing destinations, including aliases,
performs modulo-32/64-bit accumulation, and changes only N/Z when requested.
The interpreter's C/V preservation policy is retained. PC operands and repeated
long destinations remain literal. Scalar extra transfers handle both offset
forms, either direction, and legal pre/post writeback. PC destinations, invalid
writeback aliases, P=0/W=1 and reserved register-offset bits remain literal.
LDRD/STRD and odd-address halfwords also remain literal. Every scalar access
uses the existing read/write RAM witness before any architectural mutation.

The change does not enlarge retirement/device/interrupt budgets, replace guest
algorithms, alter clocks, or enable the experimental native TLB or bulk modes.
Ordinary data-cache witnesses and optional native TLB refill share the same
instruction path. Native TLB refill remains User-only: cold privileged requests
refuse, while existing privileged DREAD/DWRITE witnesses remain usable. A
refused optional bulk comparison now resumes the exact ordinary signed-byte load.

`test_arm_ram_window` checks portable admission and, on native ARM64 hosts,
compares full register/flag/memory state with `arm_step`. It covers arithmetic
edges and aliases, both memory-witness paths, sign extension, offset/index modes,
literal bases, witness boundaries, invalid forms, condition skips, missing
translations, and exact mixed ARM/Thumb loop prefixes in User and SVC modes.
Mac CI requires the native comparison marker, rather than accepting a skip.

Physical acceptance still requires same-build/checkpoint timings for demanding
general guest work and Cydia, with profiling disabled and lifecycle checks.
Correctness tests, native coverage, and removed fallbacks alone are not speed
evidence. Keep the separate bulk/TLB experiments disabled for that comparison.

The first matched physical refresh comparison did not establish a speed win:
old engine (114.519,132.312] seconds, expanded native engine
(117.590,132.317] seconds. Both used the same ready checkpoint and exact APT v3
library/repository bytes. About 23 percent fewer compact fallback retirements
did not translate into a measured wall-time improvement. The reload target
remains unmet; statistical guest-cursor attribution is the next diagnostic.
