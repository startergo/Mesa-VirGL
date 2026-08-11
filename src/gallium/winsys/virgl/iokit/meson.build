# Copyright © 2026 startergo
# SPDX-License-Identifier: MIT

# virgl_iokit_winsys — IOKit-backed virgl winsys for macOS 10.6 guest.
# Mirrors libvirglvtest's structure; replaces socket calls with IOConnectCallMethod
# against the VMQemuVGA kext.  See FB/VMVirtIOGPU.cpp selectors 0x6000-0x6009.

libvirgliokit = static_library(
  'virgliokit',
  ['virgl_iokit_winsys.c'],
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa,
                          inc_gallium, inc_gallium_aux, inc_gallium_drivers,
                          inc_virtio],
  dependencies : [dep_libvirglcommon, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
