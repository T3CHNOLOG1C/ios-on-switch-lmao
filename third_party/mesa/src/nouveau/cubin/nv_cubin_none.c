/*
 * SPDX-License-Identifier: MIT
 *
 * nv_cubin without libelf (Darwin has none): CUDA cubin import
 * (VK_NVX_binary_import, experimental in NVK) is refused instead of parsed.
 */

#include "nv_cubin.h"

#include <string.h>

bool
nv_cubin_module_init(struct nv_cubin_module *module, const void *data,
                     size_t size)
{
   memset(module, 0, sizeof(*module));
   return false;
}

void
nv_cubin_module_fini(struct nv_cubin_module *module)
{
}

const struct nv_cubin_function *
nv_cubin_module_find_function(const struct nv_cubin_module *module,
                              const char *name)
{
   return NULL;
}

const struct nv_cubin_function_param_info *
nv_cubin_function_get_param_info(const struct nv_cubin_function *function,
                                 uint32_t index)
{
   return NULL;
}
