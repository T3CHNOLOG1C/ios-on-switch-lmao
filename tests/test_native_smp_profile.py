"""Focused serialized ADT/profile checks; no kernel model or device access."""
import copy
import hashlib
import json
import struct
import unittest

from scripts import native_oled_adt as oled, native_xnu_layout as layout
from scripts import prepare_native_smp_layout as smp


def properties(data):
    parsed = layout.parse_adt(data)
    return {key: data[start:start+size] for key,(start,size) in parsed.properties.items()}


class NativeSmpProfileTests(unittest.TestCase):
    def test_existing_single_core_output_bytes_are_unchanged(self):
        # Captured from the pre-SMP serializer with the same fixed test seed.
        for flags,expected in (
            ({},'d02d8bd90916b61ab277dc8425a5c77bf11be497da17eecfe995c538996efc7a'),
            ({'userspace_fixture':True},'9250a1fe5084de6aea9c41cc2098cfb44089e785c412071e670f2dd2a31b8475'),
            ({'root_boot':True,'storage_probe':True},'cd27f35b101c569c8923da8db5c085492f162415f5196544ee0146f94fca2aa9'),
            ({'root_boot':True,'storage_probe':True,'full_ram':True},'3ec9e2aa6973a73d227eea833c35355960dbc0e498d928b8e7917c8d870969b7')):
            with self.subTest(flags=flags):
                self.assertEqual(hashlib.sha256(oled.generate_adt(bytes(range(64)),**flags)).hexdigest(),expected)

    def test_four_physical_a57s_share_cluster_cache_and_timebase(self):
        flags=dict(root_boot=True,storage_probe=True,full_ram=True)
        base=properties(oled.generate_adt(bytes(64),**flags))
        props=properties(oled.generate_adt(bytes(64),native_smp=True,**flags))
        for core in range(4):
            path=f'/cpus/cpu{core}'
            self.assertEqual(props[path,'reg'],struct.pack('<I',core))
            self.assertEqual(props[path,'compatible'],b'arm,cortex-a57\0')
            self.assertEqual(props[path,'state'],b'stopped\0' if core else b'running\0')
            self.assertEqual(props[path,'timebase-frequency'],struct.pack('<I',19200000))
            for key in ('cluster-type','die-cluster-id','l2-cache-id'):
                self.assertEqual(props[path,key],bytes(4))
            self.assertEqual(props[path,'cluster-core-id'],struct.pack('<I',core))
            self.assertNotIn((path,'l2-cache-size'),props)
            self.assertNotIn((path,'cpu-impl-reg'),props)
        self.assertEqual(props['/psci','compatible'],b'arm,psci-0.2\0')
        self.assertEqual(props['/psci','method'],b'smc\0')
        unchanged=lambda p:{key:value for key,value in p.items()
            if not key[0].startswith('/cpus/') and key[0]!='/psci'}
        self.assertEqual(unchanged(base),unchanged(props))
        self.assertEqual(props['/chosen','ioswitch-ram-ledger'],bytes(1584))
        self.assertEqual(props['/chosen/memory-map','NativeDataSeed'],bytes(16))
        self.assertEqual(props['/chosen/memory-map','TrustCache'],bytes(16))
        self.assertEqual(props['/chosen','root-matching'],oled.ROOT_MATCHING)

    def test_smp_is_explicit_full_ram_root_only(self):
        for flags in ({'native_smp':True}, {'userspace_fixture':True,'native_smp':True},
                      {'root_boot':True,'storage_probe':True,'native_smp':True},
                      {'root_boot':True,'storage_probe':True,'full_ram':True,'native_smp':1}):
            with self.subTest(flags=flags),self.assertRaises(ValueError):oled.generate_adt(bytes(64),**flags)

    def test_only_all_four_raw_profile_is_prepared(self):
        before='-v cpus=1 serial=0 ioswitch-root-boot=1 armcpu-self-ipi=1 other=kept'
        command=smp.command_line(before).split()
        self.assertEqual(command,['-v','serial=0','ioswitch-root-boot=1','armcpu-self-ipi=1',
            'other=kept','cpus=4','sheng-smp=1','ioswitch-smp=1','ioswitch-storage=raw'])
        for bad in (before+' cpumask=3',before+' cpus=4',before.replace('cpus=1','cpus=2'),
                    before+' armcpu-skip-gic=1',before+' armcpu-spi-ipi=1'):
            with self.subTest(command=bad),self.assertRaises(ValueError):smp.command_line(bad)

    def test_new_kernel_receipt_keeps_real_original_closure_and_native_policies(self):
        previous=json.loads((smp.BASE/'build-receipt.json').read_text())
        new=copy.deepcopy(previous);new['native_smp']=dict(enabled=True,max_cpus=4)
        smp.validate_receipt(new,previous)
        for key,value in (('native_smp',dict(enabled=True,max_cpus=2)),
                          ('native_storage_dma_required',False),('native_pio_fallback',True),
                          ('full_native_ram',False),('components',previous['components'][:-1])):
            bad=copy.deepcopy(new);bad[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):smp.validate_receipt(bad,previous)


if __name__=='__main__':unittest.main()
