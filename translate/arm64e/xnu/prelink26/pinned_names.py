"""Kernel functions of one kernelcache, named by hand where names.py's other sources cannot or get them wrong.

Each is identified against the same-source function in our 12377 kernel. A wrong name links a kext to the wrong
function, so they apply only to the kernelcache whose decompressed sha256 is KERNELCACHE_SHA256 (ipsw/pins.bzl).
"""

import hashlib
from pathlib import Path

KERNELCACHE_SHA256 = '250337d45598da0e5cc578413704981368afe92f84b3450dc956e21da1b21bd4'

NAMES = {
    # retainCount 1 and IOProviderPropertyMerger's vtable (its C1 twin follows at +0x28); D2 poisons the vtable
    # pointer. 24B119's AppleUSBHostMergeProperties subclasses IOProviderPropertyMerger, 21H461's subclassed IOService,
    # so iOS 17 has no import in that GOT interval to name them.
    0xFFFFFFF00A8EA254: '__ZN24IOProviderPropertyMergerC2EPK11OSMetaClass',
    0xFFFFFFF00A8EA2A4: '__ZN24IOProviderPropertyMergerD2Ev',
    # The same pair for IOMultiMemoryDescriptor (D2 with ~IOMemoryDescriptor inlined), which IOGPUFamily subclasses.
    0xFFFFFFF00A8899F4: '__ZN23IOMultiMemoryDescriptorC2EPK11OSMetaClass',
    0xFFFFFFF00A889A4C: '__ZN23IOMultiMemoryDescriptorD2Ev',
    # IOBufferMemoryDescriptor's factories, told apart by what they pass initWithPhysicalMask: alignment and mask 0
    # with the tags stored at 0x30/0x32; alignment 1 and the caller's mask; initWithGuardPages inlined (internal flag
    # 0x20, round_page, page_size, mask 0). The GOT pass put these names on the wrong functions, the pair above among
    # them.
    0xFFFFFFF00A872760: '__ZN24IOBufferMemoryDescriptor17inTaskWithOptionsEP4taskjmmjj',
    0xFFFFFFF00A872840: '__ZN24IOBufferMemoryDescriptor22inTaskWithPhysicalMaskEP4taskjyy',
    0xFFFFFFF00A872910: '__ZN24IOBufferMemoryDescriptor20inTaskWithGuardPagesEP4taskjy',
    # Five arguments handed to initWithSpecification: the SegmentOptions form.
    0xFFFFFFF00A875FB8: (
        '__ZN12IODMACommand17withSpecificationEPFbPS_NS_9Segment64EPvjEPKNS_14SegmentOptionsEjP8IOMapperS2_'
    ),
    # segments[index] = segment, return true: the segment function kexts pass to IODMACommand as data (__got).
    0xFFFFFFF00A8762CC: '__ZN12IODMACommand12OutputHost64EPS_NS_9Segment64EPvj',
    # serviceMatching(name, table) on the raw pointer, then retains the result and releases the table it consumed.
    0xFFFFFFF00A83EA78: '__ZN9IOService15serviceMatchingEPKc11OSSharedPtrI12OSDictionaryE',
    # kern_codesigning.c in source order, which AppleMobileFileIntegrity and the sandbox import. The GOT pass put these
    # names one function off (developer_mode_state's on disable_code_signing_feature, current_proc's on
    # developer_mode_state) or none: the two panics; secure_channel_shared_page's tail call (0xFFFFFFF00A66C0FC);
    # toggle_developer_mode(true), then (false); a load (developer_mode_state, which the GOT pass names); the research
    # pair (the same code, research_mode_enabled first); rem_enable's call; rem_state's load; device state; boot mode;
    # garbage collection's tail call into its body.
    0xFFFFFFF00A66C038: '_disable_code_signing_feature',
    0xFFFFFFF00A66C100: '_enable_developer_mode',
    0xFFFFFFF00A66C150: '_disable_developer_mode',
    0xFFFFFFF00A66C1B8: '_research_mode_state',
    0xFFFFFFF00A66C1F8: '_extended_research_mode_state',
    0xFFFFFFF00A66C238: '_restricted_execution_mode_enable',
    0xFFFFFFF00A66C278: '_restricted_execution_mode_state',
    0xFFFFFFF00A66C2A0: '_update_csm_device_state',
    0xFFFFFFF00A66C2E8: '_complete_security_boot_mode',
    0xFFFFFFF00A66C34C: '_garbage_collect_provisioning_profiles',
    # After set_local_signing_public_key, as in the source.
    0xFFFFFFF00A66CB40: '_get_local_signing_public_key',
    # No calls, followed by fp_getfvp's call to fp_get_ftype, as in kern_descrip.c.
    0xFFFFFFF00A5F68C0: '_fg_get_vnode',
    # current_persona_get_id (voucher, bank, release), then persona_lookup.
    0xFFFFFFF00A62AD80: '_current_persona_get',
    # panic() after OSCollection's and OSIterator's deleted destructors, between OSMetaClass::instanceConstructed and
    # __stack_chk_fail in the GOT of every kext with an abstract class (thirty of the prelinked ones). Vtables cannot
    # name it: every pure virtual slot points here.
    0xFFFFFFF00A96EE90: '___cxa_pure_virtual',
    # The OSSharedPtr wrapper after setObject(unsigned, OSSharedPtr): the same tail call through the vtable, two slots
    # further (0x100, replaceObject after merge), which IOGameControllerFamily imports.
    0xFFFFFFF00A7A4FC0: '__ZN7OSArray13replaceObjectEjRK11OSSharedPtrIK15OSMetaClassBaseE',
}


def names_for(kernelcache: str) -> dict[int, str]:
    with Path(kernelcache).open('rb') as data:
        digest = hashlib.file_digest(data, 'sha256').hexdigest()
    return NAMES if digest == KERNELCACHE_SHA256 else {}
