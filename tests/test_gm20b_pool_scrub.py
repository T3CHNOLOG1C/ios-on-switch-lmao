"""The GM20B pool's zeroing (drivers/TegraGM20B, gpu.c "pool scrub"): the CPU no longer clears the reserved pool at boot; every
page an allocation hands out is zero whether the CPU or the copy engine zeroed it; the scrubber gives a range back clean only from
its fence's interrupt pass, never waits on the CPU, and panics with one line on a range not back in time."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
GM20B = ROOT / "drivers/TegraGM20B"


def function(source, signature):
    """The body of the function whose definition starts with `signature` (not a declaration of it)."""
    starts = [i for i in range(len(source)) if source.startswith(signature, i)]
    for start in starts:
        rest = source[start + len(signature):]
        head = rest.split("{", 1)[0]
        if ";" not in head:
            return rest.split("\n}\n", 1)[0]
    raise AssertionError("no definition of " + signature)


class PoolScrubTests(unittest.TestCase):
    def setUp(self):
        self.glue = (GM20B / "shim/xnu_glue.cpp").read_text()
        self.linux = (GM20B / "shim/linux.c").read_text()
        self.gpu = (GM20B / "gpu.c").read_text()
        self.client = (GM20B / "TegraGM20BUserClient.cpp").read_text()

    def test_no_cpu_clear_of_the_reserved_pool(self):
        init = function(self.glue, 'extern "C" int nvxnu_pool_init(int *zeroed)')
        reserved = init.split("tegra-gpu-pool-pa", 1)[1].split("IOBufferMemoryDescriptor *md", 1)[0]
        self.assertNotIn("memset", reserved)
        self.assertIn("*zeroed = 0;", init)
        self.assertIn("*zeroed = 1;", init)  # the fallback still zeroes its own pages
        setup = function(self.linux, "int nvxnu_pool_setup(void)")
        self.assertIn("bitmap_set(pool_dirty, 0, pool_npages)", setup)

    def test_every_allocation_zeroes_what_a_free_left_dirty(self):
        for signature in ("struct page *alloc_pages(gfp_t gfp, unsigned int order)",
                          "void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp,",
                          "void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long long *pa)"):
            self.assertIn("pool_zero_dirty(", function(self.linux, signature), signature)
        zero = function(self.linux, "static void pool_zero_dirty(unsigned long first, unsigned long n, int all)")
        self.assertIn("test_bit(first + k, pool_dirty)", zero)
        self.assertIn("bitmap_clear(pool_dirty, first, n)", zero)
        # a plain free marks pages dirty; only a zeroed free (CPU or scrubber) marks them clean
        self.assertIn("bitmap_set(pool_dirty, first, n)", function(self.linux, "static void pool_free(unsigned long first, unsigned long n)"))
        claim = function(self.linux, "unsigned long nvxnu_pool_claim_dirty(unsigned long max_bytes, unsigned long long *pa)")
        self.assertIn("pool_dirty[w] & ~pool_map[w]", claim)
        self.assertIn("bitmap_set(pool_map, first, n)", claim)  # claimed ranges are allocated, so nobody takes them

    def test_scrubber_frees_only_from_its_fence(self):
        fenced = function(self.gpu, "static void scrub_fenced(struct tgm_chan *chan)")
        self.assertIn("nvxnu_pool_free_zeroed(", fenced)
        self.assertIn("tgm_chan_completed(chan)", fenced)
        self.assertEqual(len(re.findall(r"nvxnu_pool_free_zeroed\(", self.gpu)), 2)  # tgm_pool_free_zeroed's and the scrubber's
        submit = function(self.gpu, "static int scrub_submit(u64 pa, u64 bytes)")
        self.assertIn("SET_REMAP_CONST_A, 0)", submit)
        self.assertIn("chan_fence(chan)", submit)  # WFI, FB flush, L2 flush of dirty lines, release, nonstall interrupt
        self.assertIn("nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS)", submit)
        self.assertNotIn("nvxnu_idle", submit)  # no CPU wait for the copy engine
        deadline = function(self.gpu, "static void scrub_deadline(void *arg)")
        self.assertIn("nvxnu_panic(", deadline)
        nonstall = function(self.gpu, "static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)")
        self.assertIn("chan->kernel == CHAN_SCRUB", nonstall)

    def test_freed_pool_bos_go_to_the_gpu_first(self):
        later = function(self.client, "bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes)")
        self.assertLess(later.index("tgm_pool_scrub(pa, bytes)"), later.index("spares()"))


if __name__ == "__main__":
    unittest.main()
