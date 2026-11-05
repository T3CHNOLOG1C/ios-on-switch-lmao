"""drivers/SwitchMotion on the host: the real driver compiled against tests/switch_motion_stubs.h, over the
register-level IMU models in tests/switch_motion_harness.cpp (ICM-42607-P/ICM-42607, ICM-40607, LSM6DS3H): exact
register traffic for identify, reset, set-up, rate changes, sleep and wake; the sample path's decode, mount matrix,
device frame and scale; the off-with-one-line and lost-interrupt-panic paths; FIFO batching (watermark from the batch
interval, one status/count burst and whole-record data bursts per drain, per-sample times) and its fall-backs."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVER = ROOT / "out/native-a57/root-v1/tree/drivers/SwitchMotion/SwitchMotion.cpp"
HEADERS = ["IOKit/IOInterruptEventSource.h", "IOKit/IOLib.h", "IOKit/IOService.h", "IOKit/IOTimerEventSource.h",
           "IOKit/IOWorkLoop.h", "IOKit/IOReturn.h", "os/log.h"]


class SwitchMotionTests(unittest.TestCase):
    def test_driver_over_imu_models(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            for name in HEADERS:
                path = temp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "' + str(ROOT / "tests/switch_motion_stubs.h") + '"\n')
            binary = temp / "switch-motion"
            compile_result = subprocess.run(
                ["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-I", str(temp),
                 str(ROOT / "tests/switch_motion_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: switch motion", result.stdout)

    def test_driver_guards(self):
        """No write reaches a part before it is identified, the ICM-40607's bank-1 write is gone, and the sample path
        never waits or polls."""
        source = DRIVER.read_text()
        body = lambda signature: source.split(signature, 1)[1].split("\n}\n", 1)[0]
        identify = body("bool SwitchMotion::identify(")
        self.assertNotIn("writeRegister", identify)
        self.assertLess(identify.index("icm42607::WhoAmI"), identify.index("st::WhoAmI"))
        for gone in ("icm4Wire", "IntfConfig4", "BankSel"):
            self.assertNotIn(gone, source)
        sample = body("void SwitchMotion::sample(")
        for wait in ("IOSleep", "IODelay", "while (", "for (;;)"):
            self.assertNotIn(wait, sample)
        self.assertLess(sample.index("icm42607::IntStatusDrdy"), sample.index("icm42607::AccelData"))
        drain = body("void SwitchMotion::drainFifo(")
        for wait in ("IOSleep", "IODelay", "while (", "for (;;)"):
            self.assertNotIn(wait, drain)
        self.assertLess(drain.index("icm42607::IntStatus,"), drain.index("icm42607::FifoData"))

    def test_sensor_property_usages(self):
        """The motion services' feature report carries the Sensor-page usages 21H461's IOHIDEventDriver knows: Report
        Interval 0x30e and Report Latency 0x31b (0x316 is Reporting State, which no element takes: BatchInterval never
        reached the driver and FIFO batching never ran before r17)."""
        hid = (ROOT / "out/native-a57/root-v1/tree/drivers/SwitchTouch/SwitchHIDDevice.cpp").read_text()
        motion = hid.split("#define MOTION_DESCRIPTOR", 1)[1].split("#undef MOTION_DESCRIPTOR", 1)[0]
        self.assertIn("0x0a, 0x0e, 0x03,", motion)
        self.assertIn("0x0a, 0x1b, 0x03,", motion)
        self.assertNotIn("0x0a, 0x16, 0x03", motion)

    def test_orientation_published(self):
        """Every orientation, the detector batched like any client, and the panel's power followed."""
        source = DRIVER.read_text()
        self.assertIn("kOrientationPolicy = OrientationPolicy::All", source)
        apply = source.split("void SwitchMotion::apply(", 1)[1].split("\n}\n", 1)[0]
        self.assertNotIn("!detector)", apply)
        self.assertIn("kOrientationBatchUS", apply)
        backlight = (ROOT / "out/native-a57/root-v1/tree/drivers/SwitchBrightness/SwitchBacklight.cpp").read_text()
        self.assertEqual(backlight.count("switch_motion_display_power("), 3)  # the weak default, off and on


if __name__ == "__main__":
    unittest.main()
