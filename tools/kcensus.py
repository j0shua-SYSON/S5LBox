"""Histogram the instruction mnemonics in an iOS 8 ARMv7s kernel __TEXT.

Linear sweep, so it is approximate: data-in-text and ARM/Thumb boundaries
produce some noise. It is good enough for its only purpose, which is ordering
the interpreter work by how much of a real kernel each family actually covers,
instead of implementing the ARM ARM front to back.
"""
import collections
import os
import sys

# Derived from this file's own location rather than hardcoded: these
# scripts live in <repo>/tools/, and an absolute path here is a path a
# project rename silently rewrites. That is exactly what happened -- the
# S5LBox rename replaced the folder name inside the string and left six
# tools pointing at a directory that has never existed.
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "work", "tools", "capstone-python"))
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

TEXT_OFF, TEXT_LEN, TEXT_VA = 0x1000, 3760128 - 0x1000, 0x80001000

# These offsets describe the iOS 8 ARMv7s kernel named in the docstring, and
# ONLY that one. They are not the 7E18 ARMv6 kernel this project boots, whose
# __TEXT starts at file offset 0 and VA 0xC0008000 (see tools/kdisasm.py).
# Handing this the wrong kernel reads past __TEXT into __DATA and produces a
# mnemonic histogram of data, which looks like a result rather than an error --
# so refuse instead of guessing.
#
# A size FLOOR alone does not catch this, and I checked: the 7E18 kernel is
# 7,942,144 bytes -- larger than this window, so it sails past a minimum and
# decodes __DATA as instructions. It yields "506 distinct mnemonics", which is
# the tell, but only if somebody notices. So the check is a band.
EXPECT_END = TEXT_OFF + TEXT_LEN
EXPECT_MAX = EXPECT_END * 3 // 2

# Families the ARM1176 already provides; anything else is new work for P2.
ARMV6 = set("""
adc add adds and ands asr asrs b bl blx bx bic bics cmp cmn eor eors ldm ldmia
ldr ldrb ldrh ldrsb ldrsh lsl lsls lsr lsrs mla mov movs mrs msr mul mvn mvns
orr orrs pop push rsb rsbs sbc stm stmia str strb strh sub subs teq tst svc
nop clz rev rev16 revsh sxtb sxth uxtb uxth smull umull smlal umlal ldrd strd
ldrex strex swp cps sev wfe wfi yield it ite itt itte ittt iteee cbz cbnz
tbb tbh dmb dsb isb pld mrc mcr mrrc mcrr cdp stc ldc bfi bfc ubfx sbfx
""".split())

def main():
    raw = open(sys.argv[1], "rb").read()
    if not (EXPECT_END <= len(raw) <= EXPECT_MAX):
        sys.exit("kcensus: %s is %d bytes, outside the %d..%d band this tool's "
                 "hardcoded iOS 8 ARMv7s __TEXT window implies. It is NOT for "
                 "the 7E18 ARMv6 kernel (7,942,144 bytes, __TEXT at offset 0, "
                 "VA 0xC0008000 -- see tools/kdisasm.py); handing it that reads "
                 "__DATA as instructions and prints a histogram of data that "
                 "looks like a result. See the note beside TEXT_OFF."
                 % (sys.argv[1], len(raw), EXPECT_END, EXPECT_MAX))
    blob = raw[TEXT_OFF:TEXT_OFF + TEXT_LEN]
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md.skipdata = True
    hist = collections.Counter()
    total = 0
    for ins in md.disasm(blob, TEXT_VA):
        hist[ins.mnemonic.split('.')[0]] += 1
        total += 1
    print("decoded %d instructions over %d distinct mnemonics" % (total, len(hist)))

    new = [(n, c) for n, c in hist.most_common() if n not in ARMV6
           and not n.startswith("(")]
    covered = total - sum(c for _, c in new)
    print("already covered by the ARMv6 core: %d (%.1f%%)"
          % (covered, 100.0 * covered / total))
    print("\ntop mnemonics NOT in the ARMv6 core:")
    for n, c in new[:30]:
        print("  %-12s %8d  %5.2f%%" % (n, c, 100.0 * c / total))

main()
