"""The charging path on the host: drivers/SwitchBattery/SwitchChargePolicy.h (PDO choice, the input limit, the RDO,
BQ24193 register encodings, temperature zones and the gauge's temperature window) and the real SwitchBattery and
SwitchPD drivers, each its own translation unit over tests/switch_charging_stubs.h, behind the register-level BQ24193,
MAX17050 and BM92T36 models in tests/switch_charging_harness.cpp: boot with Nintendo's charger, temperature zones,
hot-plug (PD, plain Type-C, USB host with Hi-Z, a Mac), a refused or unanswered request, a re-balancing charger, a
contract that is ours already, a charger that loses or refuses its settings, a boost left on by an earlier boot."""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVERS = ROOT / "drivers"
BATTERY = DRIVERS / "SwitchBattery/SwitchBattery.cpp"
POLICY = DRIVERS / "SwitchBattery/SwitchChargePolicy.h"
PD = DRIVERS / "SwitchUSB/SwitchPD.cpp"
HEADERS = ["IOKit/IOLib.h", "IOKit/IOService.h", "IOKit/IOWorkLoop.h", "IOKit/IOInterruptEventSource.h",
           "IOKit/IOTimerEventSource.h", "IOKit/IOLocks.h", "IOKit/pwr_mgt/IOPM.h", "IOKit/pwr_mgt/IOPMPowerSource.h",
           "libkern/c++/OSBoolean.h", "libkern/c++/OSDictionary.h", "libkern/c++/OSNumber.h",
           "libkern/c++/OSString.h", "libkern/c++/OSSymbol.h", "os/log.h"]
SCENARIOS = ["policy", "nintendo-boot", "jeita", "hotplug", "refused", "silent", "rebalance", "ours-already",
             "settings-lost", "not-taken", "otg-left-on", "linux-left", "bad-temp",
             "lost-while-hot", "ios-requests", "temperature-hold", "power-in-wake", "sleep-windows", "role-swap",
             "mac-port"]
FLAGS = ["-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all", "-g"]


class SwitchChargingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        temp = pathlib.Path(cls.temp.name)
        for name in HEADERS:
            path = temp / "include" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "' + str(ROOT / "tests/switch_charging_stubs.h") + '"\n')
        objects = []
        for source in (BATTERY, PD, ROOT / "tests/switch_charging_harness.cpp"):
            obj = temp / (source.stem + ".o")
            result = subprocess.run(["xcrun", "clang++", *FLAGS, "-I", str(temp / "include"), "-c", str(source),
                                     "-o", str(obj)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode:
                raise AssertionError(result.stdout)
            objects.append(str(obj))
        cls.binary = temp / "switch-charging"
        result = subprocess.run(["xcrun", "clang++", "-fsanitize=address,undefined", *objects, "-o", str(cls.binary)],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            raise AssertionError(result.stdout)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_scenarios(self):
        for scenario in SCENARIOS:
            with self.subTest(scenario=scenario):
                result = subprocess.run([self.binary, scenario], text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn("PASS: switch charging " + scenario, result.stdout)

    def test_limits_are_the_cited_ones(self):
        """The limits in the policy header are the Nintendo/switchroot values (SwitchChargePolicy.h cites each)."""
        policy = POLICY.read_text()
        expected = {"kChargeCurrentMaxMA": "2048", "kChargeVoltageMaxMV": "4208", "kCoolChargeCurrentMA": "768",
                    "kPreChargeMA": "128", "kTerminationMA": "128", "kSystemMinimumMV": "3000",
                    "kInputVoltageLimitMV": "4360", "kUnknownSourceMA": "500", "kPdMaxMillivolts": "15000",
                    "kPdMaxMilliamps": "3000"}
        for name, value in expected.items():
            self.assertRegex(policy, r"constexpr unsigned " + name + r" = " + value + ";", name)
        self.assertIn("kColdBelowC = 4, kCoolBelowC = 17, kHotFromC = 51", policy)
        self.assertIn("kReg05Value = bq::kEnTerm;", policy)  # termination on, watchdog and safety timer off
        self.assertIn("millivolts == 5000 ? 2000 : millivolts == 9000 ? 2000 : millivolts == 12000 ? 1500 : 1200", policy)
        self.assertIn("kThermalRatePercent[4] = {100, 70, 50, 30}", policy)  # J120 DT charger charge-limits

    def test_driver_guards(self):
        """No waits in the PD request (every step is an ALERT or a one-shot timer); the charger's REG05 is written only
        from the policy's value; SwitchBattery never touches the BM92T36 (SwitchPD owns it)."""
        pd = PD.read_text()
        start = pd.index("// --- USB-PD request (SwitchPD.h)")
        negotiation = pd[start:pd.index("namespace {\nSwitchPD *startPD()", start)]
        for wait in ("IOSleep", "IODelay", "while (", "for (;;)", "check_delivery"):
            self.assertNotIn(wait, negotiation, wait)
        battery = BATTERY.read_text()
        # REG05 has two writers only: the managed settings (writeCharger) and the watchdog's owner (watchdogLocked).
        writes = re.findall(r"chargerUpdate\(bq::TerminationTimer, ([^;]*)\);", battery)
        self.assertEqual(writes, ["charge::kReg05Mask, s.reg05"])
        self.assertEqual(battery.count("tegra_i2c_write(kBus, kCharger, bq::TerminationTimer"), 1)
        owner = battery.split("int watchdogLocked()", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("tegra_i2c_write(kBus, kCharger, bq::TerminationTimer", owner)
        addresses = set(re.findall(r"tegra_i2c_(?:read|write)\(kBus, (\w+),", battery))
        self.assertEqual(addresses, {"kGauge", "kCharger"})


if __name__ == "__main__":
    unittest.main()
