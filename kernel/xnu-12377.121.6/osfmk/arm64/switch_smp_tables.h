/* Native XNU pmap scope only. Publish table pages, never mapped device/data leaves. */
#define SWITCH_SMP_MAX_TABLE_PAGES 8192u
struct switch_smp_table_walk {
    pmap_paddr_t ancestors[4];
    unsigned pages;
};

int switch_smp_publish_tables(void);

static int
switch_smp_clean_table(pmap_paddr_t pa, unsigned level, unsigned entries,
    const pt_attr_t *attr, struct switch_smp_table_walk *walk)
{
    const unsigned leaf = pt_attr_leaf_level(attr);
    if (level > leaf || leaf != 3 || !entries || entries > 512 || (pa & 4095) ||
        !pa_valid(pa) || !pa_valid(pa + 4095) || ++walk->pages > SWITCH_SMP_MAX_TABLE_PAGES)
        return 1;
    for (unsigned i = pt_attr_root_level(attr); i < level; ++i)
        if (walk->ancestors[i] == pa) return 1;
    walk->ancestors[level] = pa;
    const tt_entry_t *table = (const tt_entry_t *)phystokv(pa);
    CleanPoC_DcacheRegion((vm_offset_t)table, entries * sizeof(*table));
    if (level == leaf) return 0;
    const uint64_t valid = attr->pta_level_info[level].valid_mask;
    const uint64_t mask = attr->pta_level_info[level].type_mask;
    const uint64_t block = attr->pta_level_info[level].type_block;
    for (unsigned i = 0; i < entries; ++i) {
        const tt_entry_t value = __atomic_load_n(&table[i], __ATOMIC_ACQUIRE);
        if (!(value & valid) || (value & mask) == block) continue;
        if ((value & ARM_TTE_TYPE_MASK) != ARM_TTE_TYPE_TABLE ||
            switch_smp_clean_table(value & ARM_TTE_TABLE_MASK, level + 1, 512, attr, walk))
            return 1;
    }
    return 0;
}

int
switch_smp_publish_tables(void)
{
    const pt_attr_t *attr = pmap_get_pt_attr(kernel_pmap);
    if (pt_attr_page_size(attr) != 4096 || pt_attr_leaf_level(attr) != 3 ||
        pt_attr_root_level(attr) > 2 || !cpu_ttep || kernel_pmap->ttep != cpu_ttep)
        return 1;
    const vm_size_t bytes = pmap_root_alloc_size(kernel_pmap);
    if (!bytes || bytes > 4096 || bytes % sizeof(tt_entry_t)) return 1;
    struct switch_smp_table_walk walk = { { 0, 0, 0, 0 }, 0 };
    pmap_lock(kernel_pmap, PMAP_LOCK_SHARED);
    const int result = switch_smp_clean_table(cpu_ttep, pt_attr_root_level(attr),
        (unsigned)(bytes / sizeof(tt_entry_t)), attr, &walk);
    __builtin_arm_dsb(DSB_SY);
    pmap_unlock(kernel_pmap, PMAP_LOCK_SHARED);
    return result;
}
