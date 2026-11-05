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


if __name__ == "__main__":
    unittest.main()
