"""hw_test_app's results: one check per test over its `HWTEST <id> <PASS|FAIL|SKIP> <k=v...>` lines.

A test's row is PASS when the suite ran (its summary line is in the log) and the test logged no FAIL, FAIL when it
did, NO-DATA when the app never finished a run. Every check shares the literal 'HWTEST ' and the subsystem, so they add
one clause to `log show`'s predicate; their regexes tell them apart. hwtest_test.py keeps TESTS in step with the app's
sources.
"""

import re

from tools.devicerun.rules import REPORT, Check

OWNER = 'r36-hwtest'
SUBSYSTEM = 'com.brunostjohn.switch.hwtest'
SOURCE = 'hw_test_app/core/LogLine.swift'
SUITE = 'hwtest.suite'
LITERAL = 'HWTEST '

CODECS = ('h264', 'hevc', 'hevc_main10', 'vp9')
SIZES = ('720p', '1080p', '4k')
STILLS = ('jpeg_420', 'jpeg_422', 'jpeg_444', 'png', 'heic', 'heic_grid')

# The app's test IDs in its run order; True marks the interactive ones (a person plugs, presses or picks something).
TESTS: dict[str, bool] = {
    **dict.fromkeys(
        [
            'metal.device',
            'metal.compute',
            'metal.render.color',
            'metal.render.depth_stencil',
            'metal.render.msaa',
            'metal.formats',
            'metal.rgba16f_rounding',
            'metal.compressed',
            'metal.blit',
            'metal.texture_view',
            'metal.iosurface',
            'metal.mps.blur',
            'metal.mps.matmul',
            'metal.runtime_compile',
            'metal.benchmark',
        ],
        False,
    ),
    **{f'decode.{codec}_{size}': False for codec in CODECS for size in SIZES},
    'playback.h264_1080p': False,
    'playback.hevc_1080p': False,
    **{f'encode.{codec}_{preset}': False for codec in ('h264', 'hevc') for preset in ('realtime', 'quality')},
    **{f'image.{still}': False for still in STILLS},
    'image.heic_encode': False,
    'image.coreimage': False,
    'image.pixel_transfer': False,
    'audio.playback_44k': False,
    'audio.playback_48k': False,
    'audio.low_latency': False,
    'audio.route': False,
    'audio.headset': True,
    'audio.microphone': True,
    'display.mode': False,
    'display.pacing': False,
    'display.brightness': False,
    'display.colour': False,
    'sensor.accelerometer': False,
    'sensor.gyroscope': False,
    'sensor.device_motion': False,
    'sensor.ambient_light': True,
    'input.gamecontroller': True,
    'input.keyboard': True,
    'input.touch': True,
    'net.path': False,
    'net.reachability': False,
    'net.bluetooth': False,
    'net.usb_storage': True,
    'storage.sequential': False,
    'storage.random': False,
    'cpu.single': False,
    'cpu.multi': False,
    'crypto.cryptokit': False,
    'crypto.commoncrypto': False,
    'crypto.keychain': False,
    'crypto.local_authentication': False,
    'system.identity': False,
    'system.power': False,
}


def test_check(test_id: str, interactive: bool) -> Check:
    return Check(
        f'hwtest.{test_id}',
        OWNER,
        LITERAL,
        subsystem=SUBSYSTEM,
        regex=rf'^HWTEST {re.escape(test_id)} FAIL(?:$| )(?:.*\bwhy=(?P<why>\S+))?',
        show=('why',),
        needs=SUITE,
        source=SOURCE,
        note='interactive: SKIP without a person' if interactive else '',
    )


HWTEST = [
    Check(
        SUITE,
        OWNER,
        LITERAL,
        'last(fail) == 0',
        subsystem=SUBSYSTEM,
        regex=r'^HWTEST suite (?:PASS|FAIL) pass=(?P<pass>\d+) fail=(?P<fail>\d+) skip=(?P<skip>\d+)',
        show=('pass', 'fail', 'skip'),
        source=SOURCE,
        note='the last full run (Run All, or the launch run)',
    ),
    Check(
        'hwtest.skipped',
        OWNER,
        LITERAL,
        REPORT,
        subsystem=SUBSYSTEM,
        regex=r'^HWTEST (?P<id>\S+) SKIP',
        show=('id',),
        source=SOURCE,
    ),
    Check(
        'hwtest.decode-fps',
        OWNER,
        LITERAL,
        REPORT,
        subsystem=SUBSYSTEM,
        regex=r'^HWTEST (?P<id>decode\.\S+) PASS .*\bfps=(?P<fps>[\d.]+)',
        show=('id', 'fps'),
        source=SOURCE,
        note='hardware decode rate per clip',
    ),
    *[test_check(test_id, interactive) for test_id, interactive in TESTS.items()],
]
