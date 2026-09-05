# Bounded guest network profile

The bulk packet path retains the guest TCP stack and application processing.
To distinguish their remaining costs, put a nonempty `network.profile-v1` file
in a powered-off machine's working directory, then boot it normally.

The runtime samples the guest PC, LR, arguments, address space, retired slice
size and network counters at existing host service boundaries. Delivery starts
a capture. Two seconds without another delivered packet ends it and appends
`network-profile-v1.csv` in the same machine directory. File I/O does not occur
during the active transfer. Each capture holds at most 16,384 samples; excess
samples are reported as dropped. At most eight captures are written per boot.

This is a boundary profile, not unbiased instruction or wall-time sampling.
Use the timestamp and retired weights to separate idle slices from useful work,
and resolve raw addresses against the exact guest firmware. `delivered` counts
IP bytes copied to guest buffers; `emitted` counts host TCP output (including
retransmissions); `acknowledged` counts unique TCP payload cumulatively ACKed by
the guest, excluding SYN/FIN and duplicate/invalid ACKs. None alone proves that
an application received and consumed a complete download.

Without the marker there is no profile allocation or output. Move the marker
aside before the next boot for unprofiled performance measurements. Preserve
the CSV before starting another diagnostic boot, which appends to it.
