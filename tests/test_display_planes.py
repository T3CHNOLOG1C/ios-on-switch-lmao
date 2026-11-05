"""Hardware planes (NativeMobileDisplay/NativePlanes.h, r18-planes) on the host and in the driver's source.

The rules on the host (tests/display_planes_harness.cpp): every swap Core Animation can build from the published limits
accepted, window B's registers against the DC formulas, the CSC table against the float conversions, PlaneLatch against
a DC A model with the ARM copy. Source guards that the driver and XNU follow them: validateSwap through checkUpdated,
the kick on the family's effective layers with video-only swaps costing no GPU work, window B written with window A in
one STATE_CONTROL pair, retire at the latch, the panic hook taking window B down, the published limits, and backboardd
losing CA_DISABLE_DETACHED only when native_legacy_detach_ready() says so.
"""

import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
TREE = ROOT / '.'
DISPLAY = TREE / 'drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp'
PLANES = TREE / 'drivers/NativeMobileDisplay/NativePlanesDisplay.inc'
BINDINGS = TREE / 'drivers/NativeMobileDisplay/NativePlanesBindings.S'
VIC_BINDINGS = TREE / 'drivers/TegraVIC/original-bindings.S'
XNU_PATCH = ROOT / 'out/native-a57/root-v1/tree/patches/xnu-ioswitch.diff'


def body(source: str, signature: str) -> str:
    """Return the member function after `signature` (four-space class indentation), up to its closing brace."""
    return source.split(signature, 1)[1].split('\n    }\n', 1)[0]


def inc_body(source: str, signature: str) -> str:
    """Return a function of the class-body include (column 0), up to its closing brace."""
    return source.split(signature, 1)[1].split('\n}\n', 1)[0]


class DisplayPlanesTests(unittest.TestCase):
    def test_planes_on_the_host(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / 'display-planes'
            compile_result = subprocess.run(
                [
                    'xcrun',
                    'clang++',
                    '-std=c++20',
                    '-Wall',
                    '-Wextra',
                    '-Werror',
                    '-Wno-unused-parameter',
                    '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all',
                    '-g',
                    '-O1',
                    str(ROOT / 'tests/display_planes_harness.cpp'),
                    '-o',
                    str(binary),
                ],
                text=True,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run(
                [binary], check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('PASS: display planes', result.stdout)
            self.assertIn('acceptance: 200000 of 200000 swaps within the limits accepted', result.stdout)

    def test_bindings_are_tegra_vics(self):
        """The plane accessors are the IOSurface functions TegraVIC binds (its abi_check.py checks them each build)."""

        def table(path: pathlib.Path) -> dict[str, str]:
            return dict(re.findall(r'\.set _(\w+), (0x[0-9a-f]+)', path.read_text()))

        mine, vic = table(BINDINGS), table(VIC_BINDINGS)
        for name in ('plane_offset', 'plane_bytes_per_row', 'plane_height', 'plane_width'):
            self.assertEqual(mine['native_planes_surface_' + name], vic['tegra_vic_ios_' + name], name)

    def test_driver_follows_the_rules(self):
        source = DISPLAY.read_text()
        planes = PLANES.read_text()
        self.assertIn('#include "NativePlanesDisplay.inc"', source)
        # validateSwap mirrors the published limits through checkUpdated; the UI's memory is still bounded
        validate = body(source, '    bool validateSwap(const OriginalLegacySwapRequest *request) override\n')
        self.assertIn('NativePlanes::checkUpdated(', validate)
        self.assertIn('native_legacy_surface_bounds(', validate)
        # the kick reads the effective layers first; a swap that leaves window A alone changes window B only
        kick = body(source, '    void kickOnce()\n')
        self.assertLess(kick.index('planeDecide(request, pd)'), kick.index('directCandidate(pd.ui)'))
        self.assertIn('if (!pd.uiNew) {', kick)
        self.assertIn('kickPlaneOnly(request, pd', kick)
        self.assertNotIn('+ 2624', kick)  # window A's surface is the effective UI layer, not layer 0's slot
        decide = inc_body(planes, 'void planeDecide(const OriginalLegacySwapRequest *request, PlaneDecision &d)\n')
        self.assertIn('NativePlanes::kEffective', decide)
        self.assertIn('NativePlanes::rolesOf(', decide)
        # window B goes out with window A in one UPDATE write and one ACT_REQ write
        flip = body(source, '    void writeFlip(int i)\n')
        self.assertIn('planeStage(entryPlane[i], update, request)', flip)
        self.assertEqual(flip.count('dc[kDcStateControl] ='), 2)
        present = body(source, '    void presentEntry(int i)\n')
        self.assertIn('entryPlane[i].seq = entrySeq[i]', present)
        # a latched window B set retires its swaps in queue order; a stuck one or an underflow run turns planes off
        frame = inc_body(planes, 'void planeFrameEnd()\n')
        self.assertIn('retireThrough(now.seq)', frame)
        self.assertIn('planesOff(', frame)
        self.assertIn('if (status & kFrameEndInt) planeFrameEnd();', source)
        # the first client frame's swap retires at its own latch (latchRetire is on from the plane probe, before it)
        rotation = body(source, '    void startRotation()\n')
        self.assertIn('entrySeq[target] = seqOf(flipRequest);', rotation)
        self.assertIn('retireThrough(entrySeq[asked]);', body(source, '    void checkRotation()\n'))
        # swaps wait while the plane probe covers the screen; a window B set out after planes went off never panics
        self.assertIn('if (planeProbe.active()) {', kick)
        self.assertIn('!(planesOn && planeLatch.requested())', body(source, '    void checkFlipDeadline()\n'))
        # planes retire at the latch: a video's previous frame may still be scanned
        self.assertIn('latchRetire = true;', inc_body(planes, 'void planeProbeDetach()\n'))
        # the panic screen never sits under window B
        panic = source.split('void restorePitchOnPanic(const char *message)\n', 1)[1].split('\n}\n', 1)[0]
        self.assertEqual(panic.count('dc[kWinBBase + NativePlanes::kOptions] = 0;'), 2)
        # the limits Core Animation reads at the display's creation
        for key in (
            'IOMFBScalingLimits',
            'IOMFBMaxSrcPixels',
            'IOMFBNumLayers',
            'DisplayPipePlaneBaseAlignment',
            'RGBLayer_MinScaleFraction',
            'YUVLayer_MaxScale',
            'MaxSrcRectWidth',
            'MaxNumLayers',
            'PlaneBaseAlignmentLinear',
        ):
            self.assertIn(f'"{key}"', source)
        # the gate: planes on and the IOSA quarter turns (transforms 4 and 7) proven
        gate = source.split('extern "C" bool native_legacy_detach_ready(void)\n', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('planesReady()', gate)
        self.assertIn('(vic->rotations & 0x90) == 0x90', gate)

    def test_backboardd_detaches_only_when_the_display_says_so(self):
        patch = XNU_PATCH.read_text()
        self.assertIn('+native_legacy_detach_ready(void)\n+{\n+\treturn false;\n+}', patch)  # the weak default: never
        self.assertIn('+\t\tconst int single = single_layer && !native_legacy_detach_ready();', patch)
        exec_env = patch.split('+native_ioswitch_exec_environment(const struct image_params *imgp)', 1)[1].split(
            '+}\n', 1
        )[0]
        self.assertNotIn('if (single_layer) {', exec_env)
        self.assertNotIn('if (vbl && single_layer) {', exec_env)


if __name__ == '__main__':
    unittest.main()
