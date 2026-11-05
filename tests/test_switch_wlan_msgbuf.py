"""The Wi-Fi data path on the host: SwitchWLANMsgbuf.cpp against a model of the BCM4356's msgbuf firmware.

drivers/SwitchWLAN/SwitchWLANMsgbuf.cpp itself, compiled over tests/switch_wlan_msgbuf_stubs.h, runs against
tests/switch_wlan_msgbuf_harness.cpp's firmware model (rings in the DMA area, indices in the chip's RAM, the mailbox
interrupt); SwitchWLANLink.h's decoders run there too.

What r8a (11-05) measured on the device, before: at fast.com's 40 Mb/s about 1.3 frames per MSI, and 2069-3439 of
4714-6466 en0 frames per 5 s refused for want of one of 32 transmit buffers (TCP's ACKs on a download, its data on an
upload). Here: every frame of a download delivered intact with its buffer back in the next doorbell, one doorbell
and about one chip-RAM index read per batch instead of per frame, an upload of 4000 frames with no drop (frames wait
in en0's queue, a completion resumes it), the handshake path, the 512 KiB fallback, and the link decoding.
"""

import json
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
WLAN = ROOT / 'drivers/SwitchWLAN'
STUBS = ROOT / 'tests/switch_wlan_msgbuf_stubs.h'
HARNESS = ROOT / 'tests/switch_wlan_msgbuf_harness.cpp'
FLAGS = [
    '-std=c++20',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wno-unused-parameter',
    '-fsanitize=address,undefined',
    '-fno-sanitize-recover=all',
    '-g',
]
FRAME = 1514


class SwitchWlanMsgbufTests(unittest.TestCase):
    """The data path's scenarios, one harness run each."""

    @classmethod
    def setUpClass(cls) -> None:
        """Compile the driver's msgbuf unit and the harness once."""
        cls.temp = tempfile.TemporaryDirectory()
        temp = pathlib.Path(cls.temp.name)
        for name in ('IOKit/IOLib.h', 'IOKit/IOBufferMemoryDescriptor.h'):
            path = temp / 'include' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "' + str(STUBS) + '"\n')
        objects = []
        for source in (WLAN / 'SwitchWLANMsgbuf.cpp', HARNESS):
            obj = temp / (source.stem + '.o')
            result = subprocess.run(
                [
                    'xcrun',
                    'clang++',
                    *FLAGS,
                    '-I',
                    str(temp / 'include'),
                    '-I',
                    str(WLAN),
                    '-c',
                    str(source),
                    '-o',
                    str(obj),
                ],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            if result.returncode:
                raise AssertionError(result.stdout)
            objects.append(str(obj))
        cls.binary = temp / 'switch-wlan-msgbuf'
        result = subprocess.run(
            ['xcrun', 'clang++', '-fsanitize=address,undefined', *objects, '-o', str(cls.binary)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if result.returncode:
            raise AssertionError(result.stdout)

    @classmethod
    def tearDownClass(cls) -> None:
        """Remove the build directory."""
        cls.temp.cleanup()

    def run_scenario(self, *args: object) -> dict:
        """One harness scenario; its JSON line."""
        result = subprocess.run(
            [str(self.binary), *map(str, args)], check=False, text=True, capture_output=True, timeout=120
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        out = json.loads(result.stdout.strip().splitlines()[-1])
        self.assertNotIn('error', out)
        return out

    def test_attach_posts_every_receive_buffer_in_one_doorbell(self) -> None:
        """Attach posts every receive buffer in one doorbell."""
        out = self.run_scenario('attach')
        self.assertEqual(out['rx_buffers'], 255)  # the firmware's max_rxbufpost (was 64)
        self.assertEqual(out['posted_at_attach'], 255)
        self.assertEqual(out['tx_buffers'], 256)  # was 32
        self.assertEqual(out['lines'], 0)
        self.assertEqual(out['doorbells'], 1 + 16)  # the receive buffers together, then each of 16 control buffers
        self.assertIn('2048 KiB DMA area', out['describe'])
        self.assertIn('255 receive buffers posted (the firmware takes 255)', out['describe'])

    def test_download_every_frame_intact_and_buffers_back(self) -> None:
        """Download every frame intact and buffers back."""
        out = self.run_scenario('rx', 4000, 32)
        self.assertTrue(out['intact'])
        self.assertEqual(out['rx_starved'], 0)
        self.assertEqual(out['rx_posted'], out['rx_buffers'])  # every buffer back with the chip
        # One CPU copy per frame (out of the DMA buffer into the mbuf), nothing else.
        self.assertEqual(out['copied_bytes'], 4000 * FRAME)
        # Per frame before r17: 2 index reads (write index per item, read index per repost) and 1 doorbell. Now about
        # one write-index read and one doorbell per interrupt's batch of 32.
        self.assertLessEqual(out['tcm_reads'] / out['frames'], 0.15)
        self.assertLessEqual(out['doorbells'] / out['frames'], 1 / 16)
        self.assertGreaterEqual(out['frames'] / out['msis'], 16)

    def test_download_bursts_of_a_full_ampdu_never_starve_the_chip(self) -> None:
        """Download bursts of a full ampdu never starve the chip."""
        out = self.run_scenario('rx', 2048, 128)  # beyond the 64 buffers the chip had before r17
        self.assertTrue(out['intact'])
        self.assertEqual(out['rx_starved'], 0)
        self.assertGreaterEqual(out['min_posted'], 255 - 128)

    def test_upload_nothing_dropped_frames_wait_for_room(self) -> None:
        """Upload nothing dropped frames wait for room."""
        out = self.run_scenario('tx', 4000, 64, 32)
        self.assertTrue(out['intact'], out)
        self.assertEqual(out['queued_left'], 0)
        self.assertEqual(out['tx_refused'], 0)
        self.assertEqual(out['tx_in_flight'], 0)
        self.assertEqual(out['copied_bytes'], 4000 * FRAME)  # one copy each, mbuf to DMA buffer (was two)
        self.assertLessEqual(out['tx_peak'], out['tx_buffers'])
        self.assertLessEqual(out['doorbells'] / out['frames'], 1 / 8)

    def test_upload_into_a_slow_air_runs_out_and_resumes(self) -> None:
        """Upload into a slow air runs out and resumes."""
        # The air takes 4 frames a turn while TCP queued 4000: every buffer goes into flight, en0's start routine
        # stops, and completions resume it (no drop, no polling: the resume comes from the interrupt).
        out = self.run_scenario('tx', 4000, 512, 4)
        self.assertTrue(out['intact'], out)
        self.assertEqual(out['tx_peak'], out['tx_buffers'])
        self.assertGreater(out['tx_full'], 0)
        self.assertGreater(out['resumes'], 0)
        self.assertEqual(out['tx_refused'], 0)

    def test_download_with_acks_through_the_same_interrupts(self) -> None:
        """Download with acks through the same interrupts."""
        out = self.run_scenario('mixed', 4000, 32)
        self.assertEqual(out['received'], 4000)
        self.assertEqual(out['acks_seen'], out['acks'])
        self.assertEqual(out['tx_refused'], 0)
        self.assertLessEqual(out['tcm_reads'] / out['frames'], 0.3)
        self.assertLessEqual(out['doorbells'] / out['frames'], 0.25)

    def test_handshake_frame_sent_and_its_status_read(self) -> None:
        """Handshake frame sent and its status read."""
        out = self.run_scenario('eapol')
        self.assertGreater(out['id'], 0)
        self.assertFalse(out['sent_before'])
        self.assertTrue(out['sent_after'])
        self.assertEqual(out['status'], 0)
        self.assertTrue(out['seen'])

    def test_no_contiguous_2mib_takes_the_old_layout_with_one_line(self) -> None:
        """No contiguous 2mib takes the old layout with one line."""
        out = self.run_scenario('small')
        self.assertEqual(out['rx_buffers'], 64)
        self.assertEqual(out['tx_buffers'], 32)
        self.assertEqual(out['posted_at_attach'], 64)
        self.assertEqual(out['lines'], 1)
        self.assertIn('2048 KiB contiguous below 4 GiB not available', out['log'])

    def test_link_decoding(self) -> None:
        """Link decoding."""
        out = self.run_scenario('link')
        self.assertEqual(out['a'], [1, 6, 6, 20, 0])
        self.assertEqual(out['b'], [1, 149, 155, 80, 1])
        self.assertEqual(out['c'], [1, 40, 38, 40, 1])
        self.assertEqual(out['d'], [1, 8, 6, 40, 0])
        self.assertEqual(out['peer'], 'HT 2 SS 40 MHz SGI, VHT 2 SS 80 MHz SGI, HE')
        self.assertEqual(out['best80'], 866600)
        self.assertEqual(out['best20'], 173400)
        self.assertEqual(out['dump'], [1, 120, 1500, 400, 9000, 1])
        self.assertEqual(out['tenths'], '12.5')


if __name__ == '__main__':
    unittest.main()
