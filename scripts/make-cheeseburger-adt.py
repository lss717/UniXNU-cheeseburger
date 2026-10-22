#!/usr/bin/env python3
"""Generate the Apple device tree (ADT) for the OnePlus 5 port.

Not implemented yet: B2 stops at the repository skeleton because the generator needs the
device tree of the target IPSW, which the macOS/GitHub Actions kernel step fetches.

Facts it must encode for MSM8998 (measured or from the OnePlus 5 device tree):

  GICD            0x17a00000, size 0x10000
  GICR            0x17b00000, size 0x100000, stride 0x20000
  timebase        19200000
  CPU regs        0,1,2,3,0x100,0x101,0x102,0x103
  UFS node        "qcom,msm8998-ufshc" (B5 matches it; the driver owns the addresses)
  DRAM            8 GiB installed, edk2-msm8998 currently maps a 4 GiB window

The sheng generator (scripts/make-sheng-adt.py in UniXNU-sheng) is the reference: it takes
the IPSW's DeviceTree, replaces /cpus and /arm-io with the board values above, and adds
/chosen, /defaults and /options.  --identity ipad7,1 keeps the temporary B3 fixture;
--identity iphone10,2 must copy the real D21AP /product and /buttons trees (never reuse the
iPad ones).
"""

import sys


def main():
    print(__doc__.strip(), file=sys.stderr)
    print('\nmake-cheeseburger-adt.py: no output written', file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())
