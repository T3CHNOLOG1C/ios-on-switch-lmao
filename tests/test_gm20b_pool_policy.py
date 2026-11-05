"""drivers/TegraGM20B/TegraGM20BPoolPolicy.h (where a coherent BO's memory comes from) on the host, over a model of the pool at
r8a's numbers (tests/gm20b_pool_policy_harness.cpp): Maps' first buffers, refused with 33 MiB of the pool free and VectorKit
aborting on the nil buffer, now come from system memory; and the user client decides through that header, serves a BO the pool
does not from ordinary pages under a write-combined physical view, and never refuses a coherent BO for want of pool room."""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
GM20B = ROOT / "out/native-a57/root-v1/tree/drivers/TegraGM20B"


def flat(text):
    """`text` with every run of whitespace one space (the checks below read code, not its line breaks)."""
    return re.sub(r"\s+", " ", text)


def function(source, signature):
    """The body of the function whose definition starts with `signature` (not a declaration of it)."""
    for start in [i for i in range(len(source)) if source.startswith(signature, i)]:
        rest = source[start + len(signature):]
        if ";" not in rest.split("{", 1)[0]:
            return flat(rest.split("\n}\n", 1)[0])
    raise AssertionError("no definition of " + signature)


class PoolPolicyTests(unittest.TestCase):
    def test_policy_against_the_r8a_pool(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "pool-policy"
            built = subprocess.run(
                ["xcrun", "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-g", str(ROOT / "tests/gm20b_pool_policy_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(built.returncode, 0, built.stdout)
            run = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("reproduced: Maps' 4096 KiB BOs refused", run.stdout)
            self.assertIn("PASS: pool policy", run.stdout)

    def test_bo_alloc_never_refuses_a_coherent_bo_for_pool_room(self):
        client = (GM20B / "TegraGM20BUserClient.cpp").read_text()
        self.assertIn('#include "TegraGM20BPoolPolicy.h"', client)
        refusal = function(client, "const char *poolRefusal(uint64_t bytes, uint64_t held, bool compositor)")
        self.assertIn("tgm_pool::refusal(", refusal)
        self.assertIn("compositor);", refusal)  # the compositor keeps nvkm's floor
        self.assertIn("refused = poolRefusal(in->bytes, pool_bytes_, critical_);", function(client, "IOReturn AGXDeviceUserClient::boAlloc("))
        alloc = function(client, "IOReturn AGXDeviceUserClient::boAlloc(")
        self.assertIn("tgm_pool::coherentSource(refused, bo->pool_va != nullptr)", alloc)
        # the refusal no longer fails the BO: no NoMemory between the tier check and the pool allocation
        coherent = alloc.split("if (!ret && (in->flags & TegraGM20BBoCoherent)) {", 1)[1].split("if (!ret && bo->pool_va)", 1)[0]
        self.assertNotIn("kIOReturnNoMemory", coherent)
        self.assertNotIn("GPU pool BoAlloc of %llu KiB refused", client)
        # a system BO: never a spare's pages, its own size, a write-combined physical view over its pages
        self.assertIn("!ret && !system && !(in->flags & TegraGM20BBoContiguous) &&", alloc)
        self.assertIn("(in->flags & TegraGM20BBoContiguous) || system", alloc)
        view = alloc.split("if (!ret && bo->bmd && system) {", 1)[1].split("} else if (!ret && bo->bmd) {", 1)[0]
        self.assertIn("kIOMemoryTypePhysical64) | kPoolCacheMode", view)

    def test_system_bos_go_as_pool_views_do(self):
        client = (GM20B / "TegraGM20BUserClient.cpp").read_text()
        release = function(client, "void AGXDeviceUserClient::releaseBo(Bo *bo)")
        self.assertIn("if ((bo->pool_va || bo->system) && bo->view && (orphan = IONew(PoolOrphan, 1))) {", release)
        self.assertIn("bo->system ? bo->bmd : nullptr", release)
        self.assertIn("!bo->system && !(bo->flags & TegraGM20BBoContiguous) &&", release)  # never a spare
        orphans = function(client, "void releaseOrphans()")
        self.assertLess(orphans.index("orphan->view->release();"), orphans.index("orphan->backing->release();"))
        scan = function(client, "void AGXDeviceUserClient::scanPushes(")
        self.assertIn("!hit || hit->bo->system ? nullptr", scan)  # never read through the cacheable alias

    def test_stats_name_the_pool_holders(self):
        gpu = flat((GM20B / "gpu.c").read_text())
        self.assertIn("tgm_pool_breakdown(&pool_clients, &pool_orphan_bytes, &pool_orphans, &pool_system_bytes, &pool_systems);", gpu)
        self.assertIn('pool %lu/%llu MiB (compositor %llu, clients " "%llu, orphans %llu in', gpu)


if __name__ == "__main__":
    unittest.main()
