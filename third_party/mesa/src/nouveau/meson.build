# Copyright © 2021 Ilia Mirkin
# SPDX-License-Identifier: MIT

subdir('headers')
subdir('drm')
if dep_libdrm.found()
  subdir('winsys')
else
  # No libdrm (Darwin): NVK uses its darwin kernel backend; the winsys headers stay
  # visible for code that only includes them.
  idep_nouveau_ws = declare_dependency(
    include_directories : include_directories('winsys'),
  )
endif
if with_nouveau_vk
  subdir('rust')
  subdir('nil')
  subdir('compiler')
endif
if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif

if with_nouveau_vk
  subdir('cubin')
  subdir('mme')
  subdir('vulkan')
endif
