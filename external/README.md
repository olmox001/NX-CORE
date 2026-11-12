# NX-CORE external build inputs

`grub/` is the pinned GNU GRUB source used only to produce NX-CORE boot media.
It is an external Git submodule and is not linked into the kernel.  The exact
source commit and NexsOS build-pipeline revision are recorded in
`config/grub.env`.  Licensing remains the upstream GRUB license; NX-CORE's
GPL-2.0 license does not relicense this external project.
