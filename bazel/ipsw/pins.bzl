"""IPSW member pins, written by `bazel run //bazel/ipsw:pin` (bazel/ipsw/pin.py). Do not edit by hand."""

IPSW = {
    "build": "21H461",
    "device_class": "j120ap",
    "members": {
        "app_cryptex": {
            "component": "Cryptex1,AppOS",
            "content_sha256": "52e568e1eaaede5fde4684ad10afc60f8fb236bfb5a23e0869bb0ed8a1d275d7",
            "deflated": True,
            "member": "044-75188-038.dmg",
            "offset": 67,
            "sha256": "c3ef4829e3b795915aec53eeeb470ee817f97b8c27f4eaceeb6c4f18119fd6ff",
            "size": 10485760,
            "stored_size": 2781428
        },
        "app_cryptex_trust_cache": {
            "component": "Cryptex1,AppTrustCache",
            "content_sha256": "5d9bf23c900cb0b6ea241b1208a08aeef9cd863536452d85e68d1c0ef98e77c4",
            "deflated": True,
            "member": "Firmware/044-75188-038.dmg.trustcache",
            "offset": 6749437699,
            "sha256": "6244096f060c47c03b677bc1cb7e3f061bbd58fafc9a6ac144fb0544ee7f52a3",
            "size": 767,
            "stored_size": 739
        },
        "device_tree": {
            "component": "DeviceTree",
            "content_sha256": "3f612f64819fbf6a8aacc03fabeda08ba897c4bf5385df995c59815c0c80c8f0",
            "deflated": True,
            "member": "Firmware/all_flash/DeviceTree.j120ap.im4p",
            "offset": 6773560004,
            "sha256": "b6a41976d2fe70feb4d4d1a3b499df6502d9e6acbf6a91c88b27973355d096ec",
            "size": 35260,
            "stored_size": 35169
        },
        "kernelcache": {
            "component": "KernelCache",
            "content_sha256": "13b97fa2c5ce97f04408dcc28c41fb6b0d60b9440065197879acf44b2572bf7b",
            "deflated": True,
            "member": "kernelcache.release.ipad7",
            "offset": 6818903409,
            "sha256": "8c1a97e43fab66baa0be3584e933895f90cfd12ee04e2cbc0e64c3b04feb5bf3",
            "size": 16238580,
            "stored_size": 16214447
        },
        "os": {
            "component": "OS",
            "content_sha256": "3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d",
            "deflated": False,
            "member": "044-76040-038.dmg",
            "offset": 1591988020,
            "sha256": "3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d",
            "size": 5157406069,
            "stored_size": 5157406069
        },
        "static_trust_cache": {
            "component": "StaticTrustCache",
            "content_sha256": "4563bdbf6ed8f72237d45dbba3ccde432634338f95c10d79d5d4631678a97c7c",
            "deflated": True,
            "member": "Firmware/044-76040-038.dmg.trustcache",
            "offset": 6773229318,
            "sha256": "91433644b887568d5c052607aa7dd2a16eb85745378f480d5b94b7df0993bd44",
            "size": 59519,
            "stored_size": 54001
        },
        "system_cryptex": {
            "component": "Cryptex1,SystemOS",
            "content_sha256": "b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f",
            "deflated": True,
            "member": "044-76034-038.dmg",
            "offset": 270462833,
            "sha256": "ae2a7b69aaf9f58f8ebb8c9f10767b597ba5266e591f541dca0f9a56a4d394c6",
            "size": 3303014400,
            "stored_size": 1321525120
        },
        "system_cryptex_trust_cache": {
            "component": "Cryptex1,SystemTrustCache",
            "content_sha256": "9742f3d566369e6c48f63b3dc0740b06c243d13c6650e25bbf44616e13d6d097",
            "deflated": True,
            "member": "Firmware/044-76034-038.dmg.trustcache",
            "offset": 6749450558,
            "sha256": "ce0baf51e89d850811eff073f1baf75d9b6494b3ea35837f672d9c12d6821b8e",
            "size": 1871,
            "stored_size": 1748
        }
    },
    "product": "iPad7,1",
    "size": 6835124840,
    "url": "https://updates.cdn-apple.com/2025SpringFCS/fullrestores/122-77166/DCA6CFE8-90E9-4B92-880D-3DE171798E40/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw",
    "version": "17.7.11"
}
