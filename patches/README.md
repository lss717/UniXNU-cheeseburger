# Patches

`xnu/series` is the ordered board XNU patch series. It is currently empty
because device-specific kernel changes still live in the diagnostic UniXNU
fork and have not been classified into their final owners.

`unixnu/` contains reviewed compatibility patches applied by
`scripts/package.sh` to the pinned official UniXNU checkout. The current patch
exports the MSM8998 UFS probe properties through the common ADT converter.
