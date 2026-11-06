// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOSurface kext (com.apple.iokit.IOSurface, 21H461) plane accessors for the hardware planes (NativePlanes.h): the same
// functions TegraVIC binds (drivers/TegraVIC/original-bindings.S, checked against the kernelcache by its abi_check.py on every
// build), at their original addresses plus the prelink slide (-0x1000000). tests/test_display_planes.py keeps these equal
// to TegraVIC's. Each takes (IOSurface *, plane index) and returns a uint64_t.
.globl _native_planes_surface_plane_offset
.set _native_planes_surface_plane_offset, 0xfffffff004b9ed10
.globl _native_planes_surface_plane_bytes_per_row
.set _native_planes_surface_plane_bytes_per_row, 0xfffffff004b9f1fc
.globl _native_planes_surface_plane_height
.set _native_planes_surface_plane_height, 0xfffffff004b9ef84
.globl _native_planes_surface_plane_width
.set _native_planes_surface_plane_width, 0xfffffff004b9eeb8
#if XNU_12377_121_6
#error "21H461 kernelcache addresses: there are no iPadOS 27.2 (24B119) bindings for this driver yet"
#endif
