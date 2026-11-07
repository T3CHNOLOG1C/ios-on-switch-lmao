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
}


def names_for(kernelcache: str) -> dict[int, str]:
    with Path(kernelcache).open('rb') as data:
        digest = hashlib.file_digest(data, 'sha256').hexdigest()
    return NAMES if digest == KERNELCACHE_SHA256 else {}
