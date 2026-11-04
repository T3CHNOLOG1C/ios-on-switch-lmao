"""Reject inconsistent host manifests before building a freestanding adapter."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import sys as _sys; _sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parents[1] / 'tools/package'))
import build_native_loader as builder
import native_xnu_layout as layout
from tests.test_native_xnu_layout import macho, adt, trustcache, V
import native_oled_adt


class CompilerReached(RuntimeError):
    pass


class NativeLoaderBuilderTests(unittest.TestCase):
    def setUp(self):
        # Retain real safe_output confinement instead of bypassing it in tests.
        self.temp = tempfile.TemporaryDirectory(prefix="test-native-loader-", dir=builder.ROOT / "out")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        kernel, tree = self.root / "kernel.macho", self.root / "kernel.adt"
        kernel.write_bytes(macho(base=V + 0x3000))
        tree.write_bytes(adt(window=768 << 20))
        self.layout_dir = self.root / "layout"
        self.manifest = layout.build_layout(kernel, tree, self.layout_dir,
                                            physical_base=0xB2004000, window_size=768 << 20,
                                            reserved_ranges=[])
        self.payload = (self.layout_dir / "kernel-window.bin").read_bytes()
        self.output = self.root / "adapter"

    def write_manifest(self, manifest):
        (self.layout_dir / "manifest.json").write_text(json.dumps(manifest))

    def reject(self, manifest, message=None):
        self.write_manifest(manifest)
        with patch.object(builder, "tool") as tool, patch.object(builder.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, message or ".*"):
                builder.build(self.layout_dir, self.output)
            tool.assert_not_called()
            run.assert_not_called()
        self.assertFalse(self.output.exists(), "invalid manifest created adapter output")

    def test_valid_layout_reaches_compiler_only_after_validating_inputs(self):
        # Stop at the first tool lookup; no compiler executes during these tests.
        with patch.object(builder, "tool", side_effect=CompilerReached) as tool:
            with self.assertRaises(CompilerReached):
                builder.build(self.layout_dir, self.output)
            tool.assert_called_once_with("clang")
        header = (self.output / "layout.h").read_text()
        self.assertIn("#define LOAD_DEST UINT64_C(0xb2004000)", header)
        self.assertIn(f'#define LOAD_ARGS UINT64_C(0x{self.manifest["boot_args_pa"]:x})', header)
        self.assertFalse((self.output / "loader.elf").exists())
        self.assertIn('#define LOAD_NATIVE_GATES UINT64_C(0x0)', header)
        self.assertIn('#define LOAD_FAST_COPY UINT64_C(0x0)', header)

    def test_native_build_requires_reviewed_exclusions_and_video(self):
        m=deepcopy(self.manifest)
        ranges=[{'name':name,'start':start,'size':size} for name,start,size in builder.NATIVE_EXCLUSIONS]
        m['reserved_ranges']=ranges
        m['video']=[0xF5A00000,1,2880,720,1280,32]
        for key,value in [('reserved_ranges',[]),('reserved_ranges',ranges[:-1]),
                          ('reserved_ranges',ranges+[ranges[0]]),('video',[0,0,0,0,0,0])]:
            bad=deepcopy(m);bad[key]=value;self.write_manifest(bad)
            with self.subTest(key=key,value=value),patch.object(builder,'tool') as tool:
                with self.assertRaises(ValueError):
                    builder.build(self.layout_dir,self.output,native_experiment=True)
                tool.assert_not_called()
                self.assertFalse(self.output.exists())
        self.write_manifest(m)
        with patch.object(builder,'tool',side_effect=CompilerReached):
            with self.assertRaises(CompilerReached):
                builder.build(self.layout_dir,self.output,native_experiment=True)
        self.assertIn('#define LOAD_NATIVE_GATES UINT64_C(0x1)',(self.output/'layout.h').read_text())

    def test_identity_profile_and_model_ownership_mismatches_are_rejected(self):
        for key, value in (("schema_version", 2), ("profile", "native-device-loader"),
                           ("native_window_ownership_verified", True),
                           ("native_window_ownership_verified", None),
                           ("used_extent", self.manifest["used_extent"] + 8)):
            m = deepcopy(self.manifest)
            m[key] = value
            with self.subTest(key=key, value=value):
                self.reject(m)
        for key, value in (("file", "../other.bin"), ("sha256", "0" * 64),
                           ("size_bytes", len(self.payload) - 8)):
            m = deepcopy(self.manifest)
            m["image"][key] = value
            with self.subTest(image_key=key):
                self.reject(m)
        m = deepcopy(self.manifest)
        del m["native_window_ownership_verified"]
        self.reject(m, "explicitly unverified")

    def test_prelinked_profile_keeps_exact_reviewed_ownership_envelope(self):
        kernel, tree = self.root / 'prelinked.macho', self.root / 'prelinked.adt'
        kernel.write_bytes(macho(base=0xFFFFFFF00427C000))
        tree.write_bytes(adt(window=768 << 20))
        directory = self.root / 'prelinked-layout'
        manifest = layout.build_layout(kernel, tree, directory, physical_base=0xB207C000,
            window_size=768 << 20,
            reserved_ranges=[dict(name=n,start=s,size=z) for n,s,z in builder.NATIVE_EXCLUSIONS],
            video=[0xF5A00000,1,2880,720,1280,32])
        self.assertEqual(manifest['block_rounded_extent']['start'],0xB2000000)
        self.assertEqual(manifest['block_rounded_extent']['end'],0xE2200000)
        with patch.object(builder,'tool',side_effect=CompilerReached):
            with self.assertRaises(CompilerReached):
                builder.build(directory,self.output,native_experiment=True)
        self.assertIn('#define LOAD_DEST UINT64_C(0xb207c000)',(self.output/'layout.h').read_text())
        self.assertIn('#define LOAD_NATIVE_GATES UINT64_C(0x1)',(self.output/'layout.h').read_text())

    def test_storage_reference_requires_exact_pointer_sidecars_and_explicit_argument(self):
        kernel, tree, disk, cache=[self.root/n for n in ('sd-kernel','sd-tree','sd-user','sd-trust')]
        kernel.write_bytes(macho(base=0xFFFFFFF00427C000))
        tree.write_bytes(native_oled_adt.generate_adt(bytes(64),userspace_fixture=True,storage_probe=True))
        disk.write_bytes(b'own fixture');cache.write_bytes(trustcache())
        directory=self.root/'sd-layout'
        m=layout.build_layout(kernel,tree,directory,physical_base=0xB207C000,window_size=768<<20,
            reserved_ranges=[dict(name=n,start=s,size=z) for n,s,z in builder.NATIVE_EXCLUSIONS],
            video=[0xF5A00000,1,2880,720,1280,32],ramdisk_path=disk,trustcache_path=cache,
            command_line='cpus=1 ioswitch-sd-test=1')
        for command in ('cpus=1','ioswitch-sd-test=0','ioswitch-sd-test=1 ioswitch-sd-test=1'):
            bad=deepcopy(m);bad['command_line']=command
            (directory/'manifest.json').write_text(json.dumps(bad))
            with patch.object(builder,'tool') as tool,self.assertRaisesRegex(ValueError,'explicit boot argument'):
                builder.build(directory,self.output,native_experiment=True,storage_probe=True)
            tool.assert_not_called();self.assertFalse(self.output.exists())
        (directory/'manifest.json').write_text(json.dumps(m))
        bad=deepcopy(m);bad['command_line']='cpus=1 ioswitch-sd-test=1 extra-unstaged=1'
        (directory/'manifest.json').write_text(json.dumps(bad))
        with patch.object(builder,'tool') as tool,self.assertRaisesRegex(ValueError,'staged boot arguments'):
            builder.build(directory,self.output,native_experiment=True,storage_probe=True)
        tool.assert_not_called();self.assertFalse(self.output.exists())
        (directory/'manifest.json').write_text(json.dumps(m))
        with patch.object(builder,'tool') as tool,self.assertRaisesRegex(ValueError,'reference-enabled'):
            builder.build(directory,self.output,native_experiment=True)
        tool.assert_not_called()
        with patch.object(builder,'tool',side_effect=CompilerReached),self.assertRaises(CompilerReached):
            builder.build(directory,self.output,native_experiment=True,storage_probe=True)
        self.assertIn('#define LOAD_SD_REFERENCE UINT64_C(0x1)',(self.output/'layout.h').read_text())
        self.assertEqual(m['block_rounded_extent']['start'],0xB2000000)
        self.assertEqual(m['block_rounded_extent']['end'],0xE2200000)

    def test_corrupted_payload_and_nonword_payload_are_rejected(self):
        data = bytearray(self.payload)
        data[0] ^= 1
        (self.layout_dir / "kernel-window.bin").write_bytes(data)
        self.reject(deepcopy(self.manifest), "identity mismatch")
        data = self.payload + b"x"
        (self.layout_dir / "kernel-window.bin").write_bytes(data)
        m = deepcopy(self.manifest)
        m["used_extent"] = m["image"]["size_bytes"] = len(data)
        m["image"]["sha256"] = hashlib.sha256(data).hexdigest()
        self.reject(m, "aligned bounded")

    def test_storage_files_requires_v2_loader_and_exact_staged_argument(self):
        kernel, tree, disk, cache=[self.root/n for n in ('files-kernel','files-tree','files-user','files-trust')]
        kernel.write_bytes(macho(base=0xFFFFFFF00427C000))
        tree.write_bytes(native_oled_adt.generate_adt(bytes(64),userspace_fixture=True,storage_probe=True))
        disk.write_bytes(b'own fixture');cache.write_bytes(trustcache())
        directory=self.root/'files-layout'
        m=layout.build_layout(kernel,tree,directory,physical_base=0xB207C000,window_size=768<<20,
            reserved_ranges=[dict(name=n,start=s,size=z) for n,s,z in builder.NATIVE_EXCLUSIONS],
            video=[0xF5A00000,1,2880,720,1280,32],ramdisk_path=disk,trustcache_path=cache,
            command_line='cpus=1 ioswitch-sd-test=1 ioswitch-sd-files=1')
        for kwargs in ({'storage_files':True}, {'storage_probe':True},
                       {'storage_probe':True,'storage_files':1}):
            with self.subTest(kwargs=kwargs),patch.object(builder,'tool') as tool,self.assertRaises(ValueError):
                builder.build(directory,self.output,**kwargs)
            tool.assert_not_called();self.assertFalse(self.output.exists())
        with patch.object(builder,'tool',side_effect=CompilerReached),self.assertRaises(CompilerReached):
            builder.build(directory,self.output,native_experiment=True,storage_probe=True,storage_files=True)
        self.assertIn('#define LOAD_SD_CARD UINT64_C(0x1)',(self.output/'layout.h').read_text())

    def test_storage_files_requires_explicit_argument(self):
        with patch.object(builder,'tool') as tool,self.assertRaises(ValueError):
            builder.build(self.layout_dir,self.output,storage_probe=True,storage_files=True)
        tool.assert_not_called();self.assertFalse(self.output.exists())

    def test_root_boot_requires_native_data_seed_exact_uuid_and_staged_early_storage_flag(self):
        import struct
        kernel,tree,seed,cache=[self.root/n for n in ('root-kernel','root-tree','root-seed','root-trust')]
        kernel.write_bytes(macho(base=0xFFFFFFF00427C000))
        tree.write_bytes(native_oled_adt.generate_adt(bytes(64),root_boot=True,storage_probe=True))
        seed.write_bytes(struct.pack('<8sIIQQQQ',b'DBRDISK1',1,4096,1<<30,1,0,4096)+bytes(4096))
        cache.write_bytes(trustcache())
        directory=self.root/'root-layout'
        command='cpus=1 ioswitch-sd-test=1 ioswitch-sd-files=1 ioswitch-root-boot=1'
        m=layout.build_layout(kernel,tree,directory,physical_base=0xB207C000,window_size=768<<20,
            reserved_ranges=[dict(name=n,start=s,size=z) for n,s,z in builder.NATIVE_EXCLUSIONS],
            video=[0xF5A00000,1,2880,720,1280,32],data_seed_path=seed,trustcache_path=cache,
            command_line=command)
        kwargs=dict(native_experiment=True,storage_probe=True,storage_files=True,root_boot=True)
        for flag in (False,1,'yes'):
            with self.subTest(root_boot=flag),patch.object(builder,'tool') as tool,self.assertRaises(ValueError):
                builder.build(directory,self.output,**{**kwargs,'root_boot':flag})
            tool.assert_not_called();self.assertFalse(self.output.exists())
        data=(directory/'kernel-window.bin').read_bytes()
        pos=m['boot_args_pa']-m['phys_base']+0x6c
        for bad_command in (command.replace(' ioswitch-root-boot=1',''),command+' rd=md0',
                            command+' boot-uuid=bad',command+' ioswitch-user-test=1',
                            command+' ioswitch-block-test=1',command+' ioswitch-root-boot=1'):
            altered=bytearray(data);encoded=bad_command.encode();altered[pos:pos+1024]=encoded+bytes(1024-len(encoded))
            bad=deepcopy(m);bad['command_line']=bad_command;bad['image']['sha256']=hashlib.sha256(altered).hexdigest()
            (directory/'manifest.json').write_text(json.dumps(bad));(directory/'kernel-window.bin').write_bytes(altered)
            with self.subTest(command=bad_command),patch.object(builder,'tool') as tool,self.assertRaises(ValueError):
                builder.build(directory,self.output,**kwargs)
            tool.assert_not_called();self.assertFalse(self.output.exists())
        altered=bytearray(data);original=tree.read_bytes();props=layout.parse_adt(original).properties
        pointer=m['adt_pa']-m['phys_base']+props['/chosen/memory-map','NativeDataSeed'][0]
        struct.pack_into('<Q',altered,pointer,m['used_end'])
        bad=deepcopy(m);bad['image']['sha256']=hashlib.sha256(altered).hexdigest()
        (directory/'manifest.json').write_text(json.dumps(bad));(directory/'kernel-window.bin').write_bytes(altered)
        with patch.object(builder,'tool') as tool,self.assertRaisesRegex(ValueError,'sidecar bounds'):
            builder.build(directory,self.output,**kwargs)
        tool.assert_not_called();self.assertFalse(self.output.exists())
        (directory/'manifest.json').write_text(json.dumps(m));(directory/'kernel-window.bin').write_bytes(data)
        with patch.object(builder,'tool',side_effect=CompilerReached),self.assertRaises(CompilerReached):
            builder.build(directory,self.output,**kwargs)
        self.assertIn('#define LOAD_SD_CARD UINT64_C(0x1)',(self.output/'layout.h').read_text())
        self.assertIn('#define LOAD_FAST_COPY UINT64_C(0x1)',(self.output/'layout.h').read_text())
        (self.output/'layout.h').unlink()
        self.output.rmdir()

        # Inspect every planned command while never executing a compiler.
        def planned(command, **_):
            if command[0] == 'ld.lld':
                Path(command[command.index('-o')+1]).write_bytes(b'planned ELF only')
            return subprocess.CompletedProcess(command,0,'','')
        with patch.object(builder,'tool',side_effect=lambda name:name), \
             patch.object(builder.subprocess,'run',side_effect=planned) as run, \
             patch.object(builder,'validate_elf',side_effect=CompilerReached), self.assertRaises(CompilerReached):
            builder.build(directory,self.output,**kwargs)
        commands=[call.args[0] for call in run.call_args_list]
        compiled={command[command.index('-c')+1] for command in commands if '-c' in command}
        self.assertTrue({str(builder.ROOT/'native/loader/copy.c'),str(builder.ROOT/'native/loader/copy.S')} <= compiled)
        self.assertEqual(commands[-1][1:3],['-T',str(builder.ROOT/'native/loader/link.ld')])
        self.assertTrue(all(str(builder.ROOT/'native/loader') in command for command in commands[:-1]))

        # A partial cache line cannot enter the 64-byte assembly copy loop.
        import shutil
        shutil.rmtree(self.output)
        altered=data+bytes(8)
        bad=deepcopy(m)
        for key in ('used_extent','used_end','top_of_kernel_data'):
            bad[key]+=8
        bad['image']['size_bytes']=len(altered)
        bad['image']['sha256']=hashlib.sha256(altered).hexdigest()
        (directory/'manifest.json').write_text(json.dumps(bad))
        (directory/'kernel-window.bin').write_bytes(altered)
        with patch.object(builder,'tool') as tool,self.assertRaisesRegex(ValueError,'page-aligned payload'):
            builder.build(directory,self.output,**kwargs)
        tool.assert_not_called();self.assertFalse(self.output.exists())

    def test_fixed_addresses_and_pointer_relationships_are_rejected(self):
        mutations = (("phys_base", 0xB4004000), ("owned_window_size", 512 << 20),
                     ("top_of_kernel_data", self.manifest["top_of_kernel_data"] + 0x4000),
                     ("used_end", self.manifest["used_end"] + 1),
                     ("window_end", self.manifest["window_end"] - 1),
                     ("adt_va", self.manifest["adt_va"] + 8),
                     ("entry_va", self.manifest["entry_va"] + 4),
                     ("boot_args_size", 0x400),
                     ("boot_args_pa", self.manifest["boot_args_pa"] + 8),
                     ("adt_pa", self.manifest["adt_pa"] + 0x1000),
                     ("adt_size", 0), ("adt_size", len(self.payload)),
                     ("entry_pa", self.manifest["entry_pa"] + 1))
        for key, value in mutations:
            m = deepcopy(self.manifest)
            m[key] = value
            with self.subTest(key=key):
                self.reject(m)

    def test_unsigned_field_types_and_ranges_are_rejected(self):
        for value in (-1, 1 << 64, True, "0x480"):
            m = deepcopy(self.manifest)
            m["boot_args_size"] = value
            with self.subTest(value=value):
                self.reject(m, "unsigned layout field")

    def test_entry_below_payload_is_rejected_even_with_matching_virtual_entry(self):
        m = deepcopy(self.manifest)
        m["entry_pa"] = m["phys_base"] - 4
        m["entry_va"] = m["virt_base"] - 4
        self.reject(m, "outside used extent")

    def test_va_congruence_and_virtual_window_overflow_are_rejected(self):
        for base in (self.manifest["virt_base"] + 0x1000, 0xFFFFFFFFF0004000):
            m = deepcopy(self.manifest)
            m["virt_base"] = base
            m["entry_va"] = base + m["entry_pa"] - m["phys_base"]
            m["adt_va"] = base + m["adt_pa"] - m["phys_base"]
            with self.subTest(base=hex(base)):
                self.reject(m)

    def test_symlink_input_and_existing_output_are_rejected(self):
        payload = self.layout_dir / "kernel-window.bin"
        target = self.root / "payload.bin"
        payload.rename(target)
        payload.symlink_to(target)
        self.reject(deepcopy(self.manifest), "not a regular input")
        payload.unlink()
        target.rename(payload)
        self.output.mkdir()
        keep = self.output / "keep"
        keep.write_text("preserved")
        with patch.object(builder, "tool") as tool, self.assertRaisesRegex(ValueError, "new directory"):
            builder.build(self.layout_dir, self.output)
        tool.assert_not_called()
        self.assertEqual(list(self.output.iterdir()), [keep])
        self.assertEqual(keep.read_text(), "preserved")

    def test_direct_cli_help_works_outside_project_directory(self):
        result = subprocess.run([sys.executable, str(builder.ROOT / "tools/package/build_native_loader.py"), "--help"],
                                cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--layout", result.stdout)
        self.assertIn("--output", result.stdout)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
