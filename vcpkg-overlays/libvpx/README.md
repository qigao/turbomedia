# libvpx MinGW overlay

Based on vcpkg `libvpx` 1.16.0#2 at port tree
`6ceb7c67ef2fd1729a37204404ef21fc5647154f`. The native libvpx configure
script supports static linking for MinGW. This port selects that linkage
on MinGW, leaving the shared triplet available to the remaining packages.
