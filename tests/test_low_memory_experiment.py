"""Contract/confinement tests for the isolated payload experiment; no VM boots."""
from pathlib import Path
import json
import struct
import tempfile
import unittest
from unittest.mock import patch

from scripts import low_memory_experiment as experiment
from scripts import export_unixnu as exporter
from tests.test_export_unixnu import ipad_adt, adt_node


def reference_adt(**kwargs):
    tree = exporter.parse_adt(ipad_adt(**kwargs))
    tree["children"].append(exporter.parse_adt(adt_node("defaults", {"kern.maxvnodes": struct.pack("<I", 66560)})))
    tree["children"].append(exporter.parse_adt(adt_node("arm-io", {"ranges": struct.pack("<QQQ", 0, 0x8000000, 0x4000000)}, [
        adt_node("intc", {"compatible": b"arm,gic-v3\0", "reg": struct.pack("<QQ", 0, 0x10000)}),
        adt_node("virtio-mmio-0", {"compatible": b"virtio,mmio\0", "reg": struct.pack("<QQ", 0x2000000, 0x200), "dma-coherent": b""}),
    ])))
    return experiment.serialize_adt(tree)


def fdt(*, ram=4096 << 20, virtio=0xa000000, coherent=True):
    # A tiny actual-format FDT fixture, including a root, CPU, GIC and VirtIO.
    strings = bytearray()
    indices = {}
    def node(name, props, children=()):
        blob = struct.pack(">I", 1) + name.encode() + b"\0"
        blob += bytes(-len(blob) % 4)
        for key, value in props.items():
            if key not in indices:
                indices[key] = len(strings)
                strings.extend(key.encode() + b"\0")
            blob += struct.pack(">III", 3, len(value), indices[key]) + value + bytes(-len(value) % 4)
        return blob + b"".join(children) + struct.pack(">I", 2)
    mmio = {"compatible": b"virtio,mmio\0", "reg": struct.pack(">QQ", virtio, 0x200)}
    if coherent:
        mmio["dma-coherent"] = b""
    tree = node("", {"#address-cells": struct.pack(">I", 2), "#size-cells": struct.pack(">I", 2)}, [
        node("memory@40000000", {"device_type": b"memory\0", "reg": struct.pack(">QQ", 0x40000000, ram)}),
        node("cpus", {}, [node("cpu@0", {"device_type": b"cpu\0", "compatible": b"arm,cortex-a710\0"})]),
        node("intc@8000000", {"compatible": b"arm,gic-v3\0", "reg": struct.pack(">QQ", 0x8000000, 0x10000)}),
        node("virtio_mmio@a000000", mmio),
    ]) + struct.pack(">I", 9)
    return struct.pack(">10I", 0xd00dfeed, 56 + len(tree) + len(strings), 56, 56 + len(tree), 40, 17, 16, 0, len(strings), len(tree)) + bytes(16) + tree + strings


class ExperimentTests(unittest.TestCase):
    def test_exact_three_property_edit_preserves_length_and_reference(self):
        original = reference_adt()
        derivative = experiment.changed_adt(original)
        self.assertEqual(len(original), len(derivative))
        tree = experiment.validate_derivative(original, derivative)
        for node, key, width, old, new in experiment.CHANGES:
            self.assertEqual(exporter.number_property(exporter.child(tree, node), key, width), new)
            self.assertEqual(exporter.number_property(exporter.child(exporter.parse_adt(original), node), key, width), old)
        with self.assertRaises(exporter.ExportError):
            exporter.validate_adt(derivative)  # Canonical validator remains strict.

    def test_unknown_baseline_or_timer_is_rejected(self):
        for kwargs in ({"ram": 4096 << 20}, {"timer": 24000000}):
            with self.subTest(kwargs=kwargs), self.assertRaises(exporter.ExportError):
                experiment.changed_adt(reference_adt(**kwargs))
        tree = exporter.parse_adt(reference_adt())
        exporter.child(tree, "defaults")["properties"]["kern.maxvnodes"] = struct.pack("<I", 1024)
        with self.assertRaises(experiment.ExperimentError):
            experiment.changed_adt(experiment.serialize_adt(tree))

    def test_rejects_reference_or_a_fourth_changed_property(self):
        original = reference_adt()
        with self.assertRaises(experiment.ExperimentError):
            experiment.validate_derivative(original, original)
        tree = exporter.parse_adt(experiment.changed_adt(original))
        tree["properties"]["extra"] = b"unexpected"
        with self.assertRaises(experiment.ExperimentError):
            experiment.validate_derivative(original, experiment.serialize_adt(tree))

    def test_fdt_contract_checks_memory_and_mmio_and_dma(self):
        tree = exporter.parse_adt(experiment.changed_adt(reference_adt()))
        self.assertEqual(experiment.validate_fdt(fdt(), tree)["dram_bytes"], 4096 << 20)
        for kwargs in ({"ram": 6144 << 20}, {"virtio": 0xb000000}, {"coherent": False}):
            with self.subTest(kwargs=kwargs), self.assertRaises(experiment.ExperimentError):
                experiment.validate_fdt(fdt(**kwargs), tree)
        for blob in (fdt()[:20], fdt()[:-10], bytes(100)):
            with self.subTest(length=len(blob)), self.assertRaises(experiment.ExperimentError):
                experiment.parse_fdt(blob)

    def test_preparation_never_overwrites_or_nests_in_reference(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            reference = base / "reference"
            reference.mkdir()
            (reference / "keep").write_text("keep")
            with patch.object(experiment.ioswitch, "load_bundle") as load:
                for output in (reference, reference / "nested"):
                    with self.assertRaises(experiment.ExperimentError):
                        experiment.prepare(reference, output)
                load.assert_not_called()
            self.assertEqual((reference / "keep").read_text(), "keep")

    def test_fixed_command_preserves_readonly_inputs_and_reference_manifest(self):
        manifest = {"ram_mib": 6144, "cpu": "cortex-a710", "screen": "1376x1024",
                    "firmware": "code.fd", "os_disk": "os.raw", "systemos_disk": "system.raw", "esp": "esp"}
        command = experiment.experiment_command(Path("/reference"), manifest, Path("/experiment"), Path("/run"))
        self.assertEqual(command[command.index("-m") + 1], "4096")
        self.assertEqual(command[command.index("-accel") + 1], "tcg,tb-size=256")
        self.assertEqual(manifest["ram_mib"], 6144)
        self.assertIn("if=none,id=esp,format=raw,readonly=on,file=fat:ro:/experiment/esp", command)
        for argument in command:
            if "file=/reference/" in argument:
                self.assertIn("readonly=on", argument)
        with self.assertRaises(experiment.ExperimentError):
            experiment.experiment_command(Path("/reference"), manifest, Path("/bad,path"), Path("/run"))

    def test_linux_run_rejected_before_reading_any_artifacts(self):
        with patch.object(experiment.platform, "system", return_value="Linux"), patch.object(experiment, "validate_experiment") as check:
            with self.assertRaises(experiment.ExperimentError):
                experiment.run_experiment(Path("/test"))
            check.assert_not_called()

    def test_rejects_output_aliases_delimiters_and_external_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve()
            allowed = base / "experiments"
            allowed.mkdir()
            (allowed / "alias").symlink_to(base, target_is_directory=True)
            with patch.object(experiment, "EXPERIMENTS_ROOT", allowed):
                for output in (base / "outside", allowed, allowed / "bad,path", allowed / "bad\npath", allowed / "alias/child"):
                    with self.subTest(output=output), self.assertRaises(experiment.ExperimentError):
                        experiment.safe_output(output)
                self.assertEqual(experiment.safe_output(allowed / "new"), allowed / "new")

    def test_run_symlink_rejected_before_hashing_or_execution(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve()
            output = base / "experiments/test"
            output.mkdir(parents=True)
            (output / "runs").symlink_to(base, target_is_directory=True)
            with patch.object(experiment, "EXPERIMENTS_ROOT", base / "experiments"), \
                    patch.object(experiment.platform, "system", return_value="Darwin"), \
                    patch.object(experiment.platform, "machine", return_value="arm64"), \
                    patch.object(experiment, "validate_experiment") as check:
                with self.assertRaisesRegex(experiment.ExperimentError, "runs must"):
                    experiment.run_experiment(output)
                check.assert_not_called()

    def test_failed_execution_keeps_experimental_label_and_integrity_result(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp).resolve() / "experiments/test"
            run = output / "runs/one"
            run.mkdir(parents=True)
            receipt = {"reference_manifest_sha256": "test-digest"}
            def failed_execute(*args, **kwargs):
                experiment.write_json(run / "result.json", {"profile": "unixnu-ipados", "qemu_exit_code": 1})
                raise experiment.ioswitch.Error("QEMU failed")
            with patch.object(experiment, "EXPERIMENTS_ROOT", output.parent), \
                    patch.object(experiment.platform, "system", return_value="Darwin"), \
                    patch.object(experiment.platform, "machine", return_value="arm64"), \
                    patch.object(experiment, "validate_experiment", return_value=(output.parent, {}, receipt)), \
                    patch.object(experiment.ioswitch, "new_run", return_value=run), \
                    patch.object(experiment.ioswitch, "make_vars"), \
                    patch.object(experiment, "experiment_command", return_value=[]), \
                    patch.object(experiment.ioswitch, "execute", side_effect=failed_execute):
                with self.assertRaises(experiment.ioswitch.Error):
                    experiment.run_experiment(output)
            result = json.loads((run / "result.json").read_text())
            self.assertEqual(result["profile"], experiment.PROFILE)
            self.assertEqual(result["qemu_exit_code"], 1)
            self.assertTrue(json.loads((run / "post-run-integrity.json").read_text())["passed"])


if __name__ == "__main__":
    unittest.main()
