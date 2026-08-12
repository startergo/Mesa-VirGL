# Copyright © 2026 startergo
# SPDX-License-Identifier: MIT

# virgl_iokit_winsys — IOKit-backed virgl winsys for macOS 10.6 guest.
# Mirrors libvirglvtest's structure; replaces socket calls with IOConnectCallMethod
# against the VMQemuVGA kext.  See FB/VMVirtIOGPU.cpp selectors 0x6000-0x6009.

# IOKit framework — IOConnectCallMethod, IOServiceOpen, etc.
# Declared here so the dependency propagates through any non-force-load
# link path. Currently virgl enters OSMesa via -force_load (which bypasses
# meson's dependency graph), so cross-compat/build-10.6.sh also appends
# -framework IOKit explicitly. Without either, the iokit symbols resolve
# to undefined at link time.
dep_iokit = declare_dependency(link_args : ['-framework', 'IOKit'])

libvirgliokit = static_library(
  'virgliokit',
  ['virgl_iokit_winsys.c'],
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa,
                          inc_gallium, inc_gallium_aux, inc_gallium_drivers,
                          inc_virtio],
  dependencies : [dep_libvirglcommon, idep_mesautil, dep_iokit],
  gnu_symbol_visibility : 'hidden',
)
