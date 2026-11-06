/* The kmem and map-protection calls the root policy loader makes. xnu-12377 moved kmem_alloc's flags API to
 * vm_kern_xnu.h and vm_map_protect to osfmk only (MACH_KERNEL_PRIVATE, with its last two arguments swapped); there
 * the protection change is the exported vm_protect, which takes a size. */
#ifndef SWITCH_KMEM_H
#define SWITCH_KMEM_H

#include <vm/vm_kern.h>

#if XNU_12377_121_6
#include <mach/vm_map.h>
#include <vm/vm_kern_xnu.h>
#define switch_vm_map_protect(map, start, end, prot, set_max) \
    vm_protect((map), (vm_address_t)(start), (vm_size_t)((end) - (start)), (set_max), (prot))
#else
#define switch_vm_map_protect(map, start, end, prot, set_max) vm_map_protect((map), (start), (end), (prot), (set_max))
#endif

#endif
