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
    # lifs. kalloc.c after kalloc_ext and kalloc_large: the data wrapper, its shared twin, kalloc_data_require, then the
    # typed entry points; all of them call kalloc_ext (0xFFFFFFF00A12F8E4).
    0xFFFFFFF00A12FCE4: '_kalloc_data',
    0xFFFFFFF00A12FE9C: '_kalloc_type_impl',
    0xFFFFFFF00A12FF3C: '_kalloc_type_var_impl',
    # "Invalid vm_map_copy_t sz:%lld, ofst:%lld"; the copyin wrapper that makes src_destroy a boolean and tail-calls
    # vm_map_copyin_internal, as ours does; the user-flags check, ktriage and VM_FLAGS_ANYWHERE toggle of vm_allocate's
    # external entry; vm_deallocate's tail call into mach_vm_deallocate.
    0xFFFFFFF00A1F1574: '_vm_map_copy_discard',
    0xFFFFFFF00A1FC608: '_vm_map_copyin',
    0xFFFFFFF00A253B20: '_vm_allocate',
    0xFFFFFFF00A253B84: '_vm_deallocate',
    # iokit_rpc.c: after iokit_make_connect_port, the same code for the ident kobject type.
    0xFFFFFFF00A25B9F0: '_iokit_make_ident_port',
    # Before vfs_attr_pack_internal (the "com.apple.FinderInfo" packer), calling it twice.
    0xFFFFFFF00A2BF04C: '_vfs_attr_pack_ext',
    # vfs_bio.c after buf_meta_breadn: buf_bwrite(ap->a_bp), the function lifs puts in its vnop_bwrite slot.
    0xFFFFFFF00A2CEEC0: '_vn_bwrite',
    # vfs_cluster.c: advisory_read is the 0x10 wrapper calling the 0x3a0 advisory_read_ext; the GOT pass had put
    # advisory_read's name on advisory_read_ext.
    0xFFFFFFF00A2DFB78: '_advisory_read_ext',
    0xFFFFFFF00A2E0190: '_advisory_read',
    # (seq << 32) | (aux << 4) | (stat & 0xf).
    0xFFFFFFF00A2F9B38: '_vfs_resolver_result',
    # The mount helper that names "lifs", with vfs_mount_at_path's calls.
    0xFFFFFFF00A2FD974: '_vfs_mount_at_path',
    # lifs's vnode operation vector: vnop_revoke's nop (a_vp, then vn_revoke), vnop_advlock's and vnop_copyfile's
    # ENOTSUP, vfs_support.c's source order.
    0xFFFFFFF00A314920: '_nop_revoke',
    0xFFFFFFF00A314A60: '_err_advlock',
    0xFFFFFFF00A314C40: '_err_copyfile',
    # mnt_kern_flag (+116) bit 30 read; bit 29 (MNTK_AUTH_OPAQUE) and bit 30 (MNTK_AUTH_OPAQUE_ACCESS) set under the
    # mount lock; the VBLK (v_type 3) check and v_specinfo of vfs_setdevvp; the audit token copy of the context.
    0xFFFFFFF00A320EA4: '_vfs_authopaqueaccess',
    0xFFFFFFF00A320EB0: '_vfs_setauthopaque',
    0xFFFFFFF00A320F54: '_vfs_setauthopaqueaccess',
    0xFFFFFFF00A321930: '_vfs_setdevvp',
    0xFFFFFFF00A32230C: '_vfs_context_copy_audit_token',
    # kpi_vfs.c, which the ubc-like shift had one function off too: MNTK_SYSTEM (0x40) and MNTK_NOSWAP (0x80) set
    # under the mount lock; owner and group stored; snap_mp's mnt_flag (+112) given mp's security bits.
    0xFFFFFFF00A3211E4: '_vfs_setmntsystem',
    0xFFFFFFF00A321478: '_vfs_setnoswap',
    0xFFFFFFF00A3221F8: '_vfs_setowner',
    0xFFFFFFF00A322A58: '_vfs_setsnapshotmntflags',
    # kern_subr.c, adjacent: hashinit, then hashdestroy.
    0xFFFFFFF00A6467BC: '_hashinit',
    0xFFFFFFF00A6468D8: '_hashdestroy',
    # The handler of the kernel's CTLTYPE_INT "L"/"LU" oids (num_vnodes, cs_blob_size, ...): SYSCTL_LONG's.
    0xFFFFFFF00A64E00C: '_sysctl_handle_long',
    # ubc_subr.c, read from the code (v_type VREG, v_ubcinfo at +120, ui_size at +32, ui_flags at +40). The GOT pass
    # had this block shifted (ubc_setsize's name on upl_abort, getsize's on msync, ...): create_upl's vm_tag_bt and
    # ubc_create_upl_kernel calls (the GOT holds its _external alias too); ui_size; UI_ISMAPPED and
    # UI_MAPPEDWRITE out; both bits set; the "overflow detected" msync; range_op's 32-bit range check and tail call;
    # ubc_setsize's tail call into ubc_setsize_ex; upl abort and abort_range.
    0xFFFFFFF00A690F8C: '_ubc_create_upl',
    0xFFFFFFF00A690188: '_ubc_getsize',
    0xFFFFFFF00A69123C: '_ubc_is_mapped',
    0xFFFFFFF00A69129C: '_ubc_is_mapped_writable',
    0xFFFFFFF00A6906E4: '_ubc_msync',
    0xFFFFFFF00A690F2C: '_ubc_range_op',
    0xFFFFFFF00A690164: '_ubc_setsize',
    0xFFFFFFF00A68FCE4: '_ubc_setsize_ex',
    0xFFFFFFF00A6911C8: '_ubc_upl_abort',
    0xFFFFFFF00A690114: '_ubc_upl_abort_range',
    # refcnt.c: the count store; ldadd +1 with the overflow check; ldaddl then ldapr on the last reference (barrier).
    0xFFFFFFF00A7EE0B4: '_os_ref_init_count_external',
    0xFFFFFFF00A7EE0D0: '_os_ref_retain_external',
    0xFFFFFFF00A7EE1FC: '_os_ref_release_barrier_external',
    # The parent list of the vfs.lifs and vfs.apfs sysctl nodes (vfs.generic's too).
    0xFFFFFFF007BDE9A8: '_sysctl__vfs_children',
    # IOLib.cpp's data allocators in source order: IOMallocData, IOMallocZeroData, IOMallocDataShareable,
    # IOMallocZeroDataShareable, IOFreeData, IOFreeDataShareable. The first, second and fifth load one heap
    # (KHEAP_DATA_BUFFERS), these three the other (KHEAP_DATA_SHARED). The GOT pass had the private heap's names on the
    # shareable allocators: our IOMallocData and IOMallocData_external are one function, so both names fit (r34-net).
    0xFFFFFFF00A80DA8C: '_IOMallocDataShareable',
    0xFFFFFFF00A80DAD8: '_IOMallocZeroDataShareable',
    0xFFFFFFF00A80DB70: '_IOFreeDataShareable',
    # IOSerialFamily. subr_xxx.c's stubs in source order: enodev's ENODEV, enodev_strat's bare return, then enxio, enoioctl,
    # enosys, eopnotsupp, nullop, and nulldev's 0 last; the cdevsw IOSerialBSDClient registers points at them.
    0xFFFFFFF00A676794: '_enodev',
    0xFFFFFFF00A67679C: '_enodev_strat',
    0xFFFFFFF00A6767C8: '_nulldev',
    # tty.c, in IOSerialFamily's GOT between tty_unlock and wakeup in this order: ttyclose (ttyflush, then ttyfree inlined:
    # three clfree, lck_mtx_destroy, kfree), ttyfree (the same deallocation), ttymalloc (kalloc_type and the clallocs)
    # and ttyselect (selrecord).
    0xFFFFFFF00A685940: '_ttyclose',
    0xFFFFFFF00A685870: '_ttyfree',
    0xFFFFFFF00A688E50: '_ttymalloc',
    0xFFFFFFF00A687AD0: '_ttyselect',
    # IOGameControllerFamily. vm_kern.c: after vm_kernel_addrperm_external, the same code with the unslide.
    0xFFFFFFF00A1EE914: '_vm_kernel_unslide_or_perm_external',
    # The callback-pointer iterateObjects, before the block one (0xFFFFFFF00A7A64A4).
    0xFFFFFFF00A7A63B0: '__ZN12OSCollection14iterateObjectsEPvPFbS0_P8OSObjectE',
    # OSNumber.cpp after the two withNumber factories: d8 stored as is (withFloat's fcvt follows); doubleValue's ucvtf.
    0xFFFFFFF00A7D66C4: '__ZN8OSNumber10withDoubleEd',
    0xFFFFFFF00A7D67B8: '__ZNK8OSNumber11doubleValueEv',
    # OSSerializeBinary.cpp: binaryWithCapacity, then addBinary, then setIndexed (0xFFFFFFF00A7E43B0).
    0xFFFFFFF00A7E40D4: '__ZN11OSSerialize18binaryWithCapacityEjPvS0_',
    # IODataQueue's structors (its metaclass "IODataQueue" just before): retainCount 1 and the vtable; D2 poisons it.
    0xFFFFFFF00A8C01E4: '__ZN11IODataQueueC2EPK11OSMetaClass',
    0xFFFFFFF00A8C0234: '__ZN11IODataQueueD2Ev',
    # corecrypto. subrs.c's fortify checks after strnstr, each panicking with its own name ("__memcpy_chk object size
    # check failed", ...) before tail-calling memmove or memset.
    0xFFFFFFF00A264588: '___memcpy_chk',
    0xFFFFFFF00A2645D4: '___memmove_chk',
    0xFFFFFFF00A264620: '___memset_chk',
    # Data. Sysctl parents, from the oids whose oid_parent they are: the root (sysctl, kern, vm, vfs), debug
    # (lowpri_throttle_*), security (mac, lockdown_mode_state; SYSCTL_EXTENSIBLE_NODE in 13432, so AMFI, AppleImage4
    # and the sandbox add to it) and hw.features (the "features" node's oid_arg1; AMFI's allows_security_research).
    0xFFFFFFF007C56218: '_sysctl__children',
    0xFFFFFFF007C55F10: '_sysctl__debug_children',
    0xFFFFFFF007C56278: '_sysctl__security_children',
    0xFFFFFFF007BFD770: '_sysctl__hw_features_children',
    # The int behind the desired_vnodes oid, next to rootvp, between ___stack_chk_guard and _gIODTPlane in apfs's GOT.
    0xFFFFFFF00AC58DA4: '_desiredvnodes',
    # Declared before gIODefaultMatchCategoryKey (the next word), between gIOGeneralInterest and gIOParentMatchKey in
    # IOStorageFamily's GOT.
    0xFFFFFFF00AC5A968: '_gIOMatchCategoryKey',
    # The isa of the kernel's global block literals (invoke, then descriptor after it), which kexts' blocks use too.
    0xFFFFFFF00AC5CA78: '__NSConcreteGlobalBlock',
    # Each 16 bytes before its class's vtable, after the class's gMetaClass in the kexts' GOTs: Class::metaClass.
    0xFFFFFFF007C19348: '__ZN13IOCommandPool9metaClassE',
    0xFFFFFFF007C19688: '__ZN28IOFilterInterruptEventSource9metaClassE',
    0xFFFFFFF007C19C68: '__ZN12IODMACommand9metaClassE',
    # image4_v2.c: "libImage4_V2 interface already set", Image4's registration.
    0xFFFFFFF00A7F8828: '_image4_v2_interface_register',
    # amfi.c's two after it, by their panics: "AppleMobileFileIntegrity interface already set", then
    # "libCoreEntitlements interface already set". The GOT pass had them one off (arp_lookup_ip's name on the first).
    0xFFFFFFF00A7F8868: '_amfi_interface_register',
    0xFFFFFFF00A7F88A8: '_amfi_core_entitlements_register',
    # kern_codesigning.c after the image4 dispatcher ("image4_ dispatch: overflow on input"), in source order: the
    # object calls (two TXM selectors each), then get_monitor_exports and set_release_type (one call each).
    0xFFFFFFF00A66D29C: '_kernel_image4_monitor_trap',
    0xFFFFFFF00A66D468: '_kernel_image4_execute_object',
    0xFFFFFFF00A66D4E0: '_kernel_image4_copy_object',
    0xFFFFFFF00A66D54C: '_kernel_image4_get_monitor_exports',
    0xFFFFFFF00A66D568: '_kernel_image4_set_release_type',
    # IOKitBSDInit.cpp after IOBSDGetPlatformUUID: IOBSDMountChange and IOBSDLowSpaceUnlinkKernelCore (empty without
    # IOPOLLED_COREFILE: a lone RET each), then the string-entitlement checks (get_bsdtask_info).
    0xFFFFFFF00A8ECC10: '_IOBSDMountChange',
    0xFFFFFFF00A8ECC14: '_IOBSDLowSpaceUnlinkKernelCore',
    0xFFFFFFF00A8ECC94: '_IOTaskHasStringEntitlement',
    # ether_if_module.c around ether_demux ("dropping short VLAN packet"): del_proto, add_proto (bcmp, bzero), demux;
    # ether_frameout's bounds checks before its tail call into ether_frameout_extended; ether_check_multi's AF_LINK
    # test (EAFNOSUPPORT, EADDRNOTAVAIL).
    0xFFFFFFF00A3845A8: '_ether_del_proto',
    0xFFFFFFF00A3846AC: '_ether_add_proto',
    0xFFFFFFF00A384B08: '_ether_demux',
    0xFFFFFFF00A385304: '_ether_frameout',
    0xFFFFFFF00A3855CC: '_ether_check_multi',
    # IODMACommand.cpp's segment functions in source order: Host32 (two 32-bit stores), Big32 (rev), Little32 (stp);
    # Little64 after the pinned Host64 and Big64.
    0xFFFFFFF00A87311C: '__ZN12IODMACommand12OutputHost32EPS_NS_9Segment64EPvj',
    0xFFFFFFF00A873134: '__ZN12IODMACommand11OutputBig32EPS_NS_9Segment64EPvj',
    0xFFFFFFF00A873154: '__ZN12IODMACommand14OutputLittle32EPS_NS_9Segment64EPvj',
    0xFFFFFFF00A8762FC: '__ZN12IODMACommand14OutputLittle64EPS_NS_9Segment64EPvj',
    # IOMemoryCursor.cpp: each cursor's outputSegment between its initWithSpecification and getMetaClass.
    0xFFFFFFF00A8797CC: '__ZN21IONaturalMemoryCursor13outputSegmentEN14IOMemoryCursor15PhysicalSegmentEPvj',
    0xFFFFFFF00A879E4C: '__ZN17IOBigMemoryCursor13outputSegmentEN14IOMemoryCursor15PhysicalSegmentEPvj',
    0xFFFFFFF00A87A4D8: '__ZN20IOLittleMemoryCursor13outputSegmentEN14IOMemoryCursor15PhysicalSegmentEPvj',
    # apfs. vfs_support.c's nop_*/err_* in source order between named ones (nop returns 0, err ENOTSUP: the
    # same words as ours).
    0xFFFFFFF00A314948: '_nop_mmap',
    0xFFFFFFF00A314950: '_err_mmap',
    0xFFFFFFF00A314958: '_nop_fsync',
    0xFFFFFFF00A314970: '_err_remove',
    0xFFFFFFF00A314980: '_err_link',
    0xFFFFFFF00A314990: '_err_rename',
    0xFFFFFFF00A3149A0: '_err_mkdir',
    0xFFFFFFF00A3149B0: '_err_rmdir',
    0xFFFFFFF00A3149C0: '_err_symlink',
    0xFFFFFFF00A3149D0: '_err_readdir',
    0xFFFFFFF00A3149F0: '_err_readdirattr',
    0xFFFFFFF00A314A10: '_err_readlink',
    0xFFFFFFF00A314A18: '_nop_inactive',
    0xFFFFFFF00A314A40: '_err_strategy',
    0xFFFFFFF00A314A48: '_nop_pathconf',
    0xFFFFFFF00A314A68: '_nop_allocate',
    0xFFFFFFF00A314A90: '_err_bwrite',
    0xFFFFFFF00A314A98: '_nop_pagein',
    0xFFFFFFF00A314B58: '_nop_pageout',
    0xFFFFFFF00A314C28: '_err_searchfs',
    # apfs. vfs_cprotect.c in source order between cpx_key and doc_tombstone_get, the same count as ours (the
    # accessors' words match).
    0xFFFFFFF00A32D0C8: '_cpx_set_is_sep_wrapped_key',
    0xFFFFFFF00A32D0DC: '_cpx_is_composite_key',
    0xFFFFFFF00A32D114: '_cpx_set_use_offset_for_iv',
    0xFFFFFFF00A32D140: '_cpx_set_synthetic_offset_for_iv',
    0xFFFFFFF00A32D160: '_cpx_max_key_len',
    0xFFFFFFF00A32D170: '_cpx_set_key_len',
    0xFFFFFFF00A32D1A4: '_cpx_set_aes_iv_key',
    0xFFFFFFF00A32D338: '_cpx_copy',
    0xFFFFFFF00A32D6F8: '_cp_is_valid_class',
    0xFFFFFFF00A32D71C: '_cp_os_version',
    # Paired in order inside runs of named neighbours that hold as many functions in 24B119 as in ours (unload_trust_cache's
    # entitlement string, vnode_addfsref's panic, the proc_ accessors' loads).
    0xFFFFFFF00A2F3F50: '_vnode_addfsref',
    0xFFFFFFF00A317B5C: '_utf8_normalizeOptCaseFoldToUTF8',
    0xFFFFFFF00A5EF578: '_current_cached_proc_cred',
    0xFFFFFFF00A612960: '_ctl_name_by_id',
    0xFFFFFFF00A629FD4: '_proc_persona_get',
    0xFFFFFFF00A62A400: '_persona_lookup',
    0xFFFFFFF00A62E624: '_proc_original_ppid',
    0xFFFFFFF00A62E7A8: '_proc_orig_ppidversion',
    0xFFFFFFF00A62EB70: '_proc_is_simulated',
    0xFFFFFFF00A62F26C: '_proc_signal',
    0xFFFFFFF00A635AB4: '_proc_pgrpid',
    0xFFFFFFF00A66B0D8: '_unload_trust_cache',
    # The sandbox's (AMFI shares some), each placed by what it does and checked against the alphabetical order of every
    # GOT holding it: kern_cs.c's csblob/csproc accessors (cs_blob grew 0x30 after csb_hashtype, so platform_binary reads
    # 0xdc), kern_proc.c's syscall-filter and sandbox-info setters (nsysent 558, mach_trap_count 128, mach_kobj_count),
    # proc_is_translated (P_TRANSLATED), proc_is_rsr, proc_ucred (p_proc_ro checked in the RO zone), file_socket
    # (DTYPE_SOCKET, ENOTSOCK) and file_drop, vnode_authorize (kauth_authorize_action), psignal's SIGKILL try-thread form,
    # PE_parse_boot_arg_str (force-string parse of the boot args), task_is_app_suspended (pidsuspended),
    # ptrauth_utils_sign_blob_generic (what ptrauth_utils_auth_blob_generic compares against).
    0xFFFFFFF00A16FB0C: '_task_is_app_suspended',
    0xFFFFFFF00A1F7998: '_vm_map_page_mask',
    0xFFFFFFF00A27CB20: '_read_frandom',
    0xFFFFFFF00A2F0524: '_vn_revoke',
    0xFFFFFFF00A2F177C: '_vn_getpath_ext_with_mntlen',
    0xFFFFFFF00A2F1860: '_vn_getpath_ext',
    0xFFFFFFF00A2F7554: '_vnode_authorize',
    0xFFFFFFF00A5F100C: '_kauth_proc_label_update',
    0xFFFFFFF00A5F1A74: '_csblob_get_flags',
    0xFFFFFFF00A5F1A94: '_csproc_get_blob',
    0xFFFFFFF00A5F1B5C: '_csvnode_get_blob',
    0xFFFFFFF00A5F1B74: '_csblob_get_identity',
    0xFFFFFFF00A5F1BD4: '_csblob_get_cdhash',
    0xFFFFFFF00A5F21E4: '_csproc_get_platform_binary',
    0xFFFFFFF00A5F2EA4: '_cs_restricted',
    0xFFFFFFF00A5F70D4: '_file_socket',
    0xFFFFFFF00A5F75C0: '_file_drop',
    0xFFFFFFF00A62D3CC: '_proc_find_ident_validated',
    0xFFFFFFF00A62F5D0: '_proc_ucred',
    0xFFFFFFF00A630104: '_proc_getexecutablevnode_noblock',
    0xFFFFFFF00A632FB8: '_proc_is_translated',
    0xFFFFFFF00A6331DC: '_proc_is_rsr',
    0xFFFFFFF00A636034: '_proc_get_syscall_filter_mask_size',
    0xFFFFFFF00A636064: '_proc_get_syscall_filter_mask',
    0xFFFFFFF00A636164: '_proc_set_syscall_filter_mask',
    0xFFFFFFF00A636294: '_proc_set_syscall_filter_callbacks',
    0xFFFFFFF00A6362F4: '_proc_set_syscall_filter_index',
    0xFFFFFFF00A636390: '_proc_set_sandbox_info_callbacks',
    0xFFFFFFF00A6363D4: '_proc_set_filter_message_flag',
    0xFFFFFFF00A643868: '_psignal_sigkill_try_thread_with_reason',
    0xFFFFFFF00A64E408: '_sysctl_handle_string',
    0xFFFFFFF00A7DFE48: '__ZN8OSSymbol24existingSymbolForCStringEPKc',
    0xFFFFFFF00A922530: '_PE_parse_boot_arg_str',
    0xFFFFFFF00A0DD2F8: '_ptrauth_utils_sign_blob_generic',
    # AMFI's: kern_cs.c's csblob accessors in source order (cs_blob grew 0x30 after csb_hashtype: base_offset 0x28, size
    # 0x40, addr 0x50 as ours; teamid 0xb8, validation_category 0xe0, code_directory 0xb0 = ours + 0x30), the csproc/csfg
    # getters and cs_require_lv/cs_system_require_lv (release constants in 24B119; ours keep their policy globals),
    # ubc_subr.c's blob parsers ("CS Blob length"), the trust-cache queries' _with_length forms (query type or cdhash
    # first), csblob_register_profile (a cs_profile_register_t: a no-op without a code-signing monitor, as ours),
    # vm_map_copyout (NULL copy), vm_map_wire (vm_tag_bt) and unwire, the printable vnode names, task_set_cs_auxiliary_info,
    # launch_constraint_data_get_launch_type, ptrauth_utils_auth_blob_generic and pmap's CoreEntitlements acceleration
    # buffers. Every one sorts between its named neighbours in the GOTs that hold it.
    0xFFFFFFF00A171B78: '_task_set_cs_auxiliary_info',
    0xFFFFFFF00A1FAF4C: '_vm_map_unwire',
    0xFFFFFFF00A1FAF58: '_vm_map_wire',
    0xFFFFFFF00A1FC5EC: '_vm_map_copyout',
    0xFFFFFFF00A2D1E94: '_vnode_getname_printable',
    0xFFFFFFF00A2D1FFC: '_vnode_putname_printable',
    0xFFFFFFF00A5F17E0: '_cs_require_lv',
    0xFFFFFFF00A5F17E8: '_cs_system_require_lv',
    0xFFFFFFF00A5F17F0: '_csblob_get_base_offset',
    0xFFFFFFF00A5F17F8: '_csblob_get_size',
    0xFFFFFFF00A5F1800: '_csblob_get_addr',
    0xFFFFFFF00A5F1808: '_csblob_get_platform_binary',
    0xFFFFFFF00A5F18D0: '_csvnode_invalidate_flags',
    0xFFFFFFF00A5F1A7C: '_csblob_get_hashtype',
    0xFFFFFFF00A5F1B6C: '_csblob_get_teamid',
    0xFFFFFFF00A5F1BDC: '_csblob_get_cdhash_with_length',
    0xFFFFFFF00A5F1BF0: '_csblob_get_signer_type',
    0xFFFFFFF00A5F1BF8: '_csblob_set_validation_category',
    0xFFFFFFF00A5F1C8C: '_csblob_get_validation_category',
    0xFFFFFFF00A5F1C94: '_csblob_set_auxiliary_info',
    0xFFFFFFF00A5F1CE8: '_csblob_get_auxiliary_info',
    0xFFFFFFF00A5F1CF0: '_csblob_get_trust_level',
    0xFFFFFFF00A5F1D2C: '_csblob_get_code_directory',
    0xFFFFFFF00A5F1DBC: '_csblob_os_entitlements_set',
    0xFFFFFFF00A5F1E28: '_csblob_os_entitlements_copy',
    0xFFFFFFF00A5F1EC8: '_csblob_os_entitlements_get',
    0xFFFFFFF00A5F1F30: '_csproc_get_teamid',
    0xFFFFFFF00A5F28EC: '_csfg_get_teamid',
    0xFFFFFFF00A5F2A4C: '_csfg_get_csblob',
    0xFFFFFFF00A5F2DF8: '_csproc_get_platform_identifier',
    0xFFFFFFF00A5F32E0: '_launch_constraint_data_get_launch_type',
    0xFFFFFFF00A66B764: '_query_trust_cache_with_length',
    0xFFFFFFF00A66B8D0: '_query_trust_cache_for_rem_with_length',
    0xFFFFFFF00A66CC7C: '_csm_resolve_os_entitlements_from_proc',
    0xFFFFFFF00A68F01C: '_csblob_find_blob_bytes',
    0xFFFFFFF00A68F180: '_csblob_get_entitlements',
    0xFFFFFFF00A68F344: '_csblob_get_der_entitlements',
    0xFFFFFFF00A68F4EC: '_csblob_register_profile',
    0xFFFFFFF00A7F88E8: '_ptrauth_utils_auth_blob_generic',
    0xFFFFFFF00A9688FC: '_pmap_ce_allocate_acceleration_buffer',
    0xFFFFFFF00A968934: '_pmap_ce_free_acceleration_buffer',
}


def names_for(kernelcache: str) -> dict[int, str]:
    with Path(kernelcache).open('rb') as data:
        digest = hashlib.file_digest(data, 'sha256').hexdigest()
    return NAMES if digest == KERNELCACHE_SHA256 else {}
