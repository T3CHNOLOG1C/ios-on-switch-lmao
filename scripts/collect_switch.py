#!/usr/bin/env python3
"""Read-only Switchroot inventory, Python 3.8+; no installs, network or VM creation.

Only allowlisted facts are emitted. Raw command output, hostnames, serials,
UUIDs, environment variables, usernames, swap paths and storage paths are not.
Each external command has a 15-second deadline and a bounded stdout buffer.
The only requested write is --output NEWFILE, opened exclusively.
"""

import argparse
import errno
import gzip
import json
import os
from pathlib import Path
import platform
import re
import selectors
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import time
import zlib

FILE_LIMIT = 2 * 1024 * 1024
COMMAND_LIMIT = 4 * 1024 * 1024
COMMAND_TIMEOUT = 15
KVM_GET_API_VERSION = 0xAE00  # Linux _IO(KVMIO, 0x00); query only, no KVM_CREATE_VM.
CONFIG_KEYS = ("CONFIG_VIRTUALIZATION", "CONFIG_KVM", "CONFIG_HAVE_KVM",
               "CONFIG_KVM_ARM_HOST", "CONFIG_KVM_ARM_VGIC", "CONFIG_ARM64_VHE")
VK_FEATURES = ("timelineSemaphore", "bufferDeviceAddress", "shaderInt64", "shaderFloat16",
               "storageBuffer16BitAccess", "uniformAndStorageBuffer16BitAccess",
               "storageBuffer8BitAccess", "uniformAndStorageBuffer8BitAccess",
               "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat")
VK_EXTENSIONS = ("VK_EXT_external_memory_host", "VK_KHR_external_memory_fd",
                 "VK_KHR_timeline_semaphore", "VK_KHR_buffer_device_address",
                 "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd")
MM_FIELDS = ("orig_data_size", "compr_data_size", "mem_used_total", "mem_limit",
             "mem_used_max", "same_pages", "pages_compacted", "huge_pages", "huge_pages_since")


def error_kind(error):
    return errno.errorcode.get(getattr(error, "errno", None), "read_error")


def safe_label(value):
    """Keep short labels, rejecting paths, addresses and identifiers within them."""
    value = value.strip().strip('"')
    if not value or len(value) > 256 or any(ord(c) < 32 for c in value):
        return None
    if any(c in value for c in ("/", "\\", "@")):
        return None
    if re.search(r"\b[0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}\b", value):
        return None
    if re.search(r"\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b", value):
        return None
    if re.search(r"\b(?:\d{1,3}\.){3}\d{1,3}\b", value):
        return None
    return value


def read_text(path, compressed=False):
    try:
        opener = gzip.open if compressed else open
        with opener(path, "rb") as stream:
            data = stream.read(FILE_LIMIT + 1)
        if len(data) > FILE_LIMIT:
            return None, "size_limit"
        return data.decode("utf-8", errors="replace"), "ok"
    except (OSError, EOFError, zlib.error) as error:
        return None, error_kind(error)


def parse_os_release(text):
    result = {}
    allowed = {"ID", "ID_LIKE", "NAME", "VERSION_ID", "VERSION_CODENAME"}
    for line in text.splitlines():
        key, separator, value = line.partition("=")
        if not separator or key not in allowed:
            continue
        try:
            parts = shlex.split(value)
        except ValueError:
            continue
        if len(parts) == 1:
            result[key] = safe_label(parts[0])
    return result


def parse_cpuinfo(text):
    processors = []
    for block in re.split(r"\n\s*\n", text.strip()):
        fields = {}
        for line in block.splitlines():
            key, separator, value = line.partition(":")
            if separator:
                fields[key.strip().lower()] = value.strip()
        if not any(key in fields for key in ("processor", "features", "flags", "cpu part")):
            continue
        features = fields.get("features", fields.get("flags"))
        processors.append({
            "index": int(fields["processor"]) if fields.get("processor", "").isdigit() else None,
            "features": sorted(set(re.findall(r"\b[a-zA-Z0-9_]+\b", features))) if features is not None else None,
            "implementer": fields.get("cpu implementer") if re.fullmatch(r"0x[0-9a-fA-F]+", fields.get("cpu implementer", "")) else None,
            "part": fields.get("cpu part") if re.fullmatch(r"0x[0-9a-fA-F]+", fields.get("cpu part", "")) else None,
        })
    return processors


def parse_meminfo(text):
    keys = {"MemTotal": "total_bytes", "MemAvailable": "available_bytes", "MemFree": "free_bytes",
            "SwapTotal": "swap_total_bytes", "SwapFree": "swap_free_bytes"}
    result = dict.fromkeys(keys.values())
    for line in text.splitlines():
        match = re.fullmatch(r"(\w+):\s+(\d+)\s+kB\s*", line)
        if match and match[1] in keys:
            result[keys[match[1]]] = int(match[2]) * 1024
    return result


def parse_swaps(text):
    lines = text.splitlines()
    if not lines or lines[0].split() != ["Filename", "Type", "Size", "Used", "Priority"]:
        return None
    result = []
    for line in lines[1:]:
        fields = line.split()
        if len(fields) != 5 or fields[1] not in ("file", "partition"):
            continue
        if not (fields[2].isdigit() and fields[3].isdigit() and re.fullmatch(r"-?\d+", fields[4])):
            continue
        result.append({"type": fields[1], "size_bytes": int(fields[2]) * 1024,
                       "used_bytes": int(fields[3]) * 1024, "priority": int(fields[4])})
    return result


def parse_kernel_config(text):
    result = dict.fromkeys(CONFIG_KEYS)
    for key in CONFIG_KEYS:
        found = re.search(r"^" + key + r"=([ymn])$", text, re.M)
        if found:
            result[key] = found[1]
        elif re.search(r"^# " + key + r" is not set$", text, re.M):
            result[key] = "n"
    return result


def collect_config(root, kernel):
    candidates = [(root / "proc/config.gz", True, "proc_config_gz")]
    if re.fullmatch(r"[A-Za-z0-9_.+\-]{1,256}", kernel):
        candidates.append((root / "boot" / ("config-" + kernel), False, "boot_config"))
    for path, compressed, source in candidates:
        data, status = read_text(path, compressed)
        if status == "ok":
            return {"status": "ok", "source": source, "flags": parse_kernel_config(data)}
    return {"status": "unavailable", "source": None, "flags": dict.fromkeys(CONFIG_KEYS)}


def collect_zram(root):
    directory = root / "sys/block"
    try:
        paths = sorted(p for p in directory.iterdir() if re.fullmatch(r"zram\d+", p.name))
    except OSError as error:
        return {"status": "unavailable", "reason": error_kind(error), "devices": None}
    devices = []
    for path in paths:
        size, size_status = read_text(path / "disksize")
        metrics, metrics_status = read_text(path / "mm_stat")
        compressor, compressor_status = read_text(path / "comp_algorithm")
        numbers = metrics.split() if metrics is not None else []
        parsed = list(map(int, numbers)) if numbers and all(n.isdigit() for n in numbers) else []
        selected = re.search(r"\[([a-zA-Z0-9_-]+)\]", compressor or "")
        devices.append({
            "index": int(path.name[4:]),
            "disk_size_bytes": int(size.strip()) if size and size.strip().isdigit() else None,
            "disk_size_status": size_status,
            "mm_stat_status": metrics_status if parsed or metrics is None else "unrecognized_output",
            "mm_stat": {key: parsed[i] if i < len(parsed) else None for i, key in enumerate(MM_FIELDS)},
            "compressor_status": compressor_status,
            "active_compressor": selected[1] if selected else None,
        })
    return {"status": "ok", "devices": devices}


def collect_kvm(root):
    path = root / "dev/kvm"
    report = {"exists": None, "mode": None, "readable": None, "writable": None,
              "api_version": None, "api_query_status": "unavailable", "vm_created": False}
    try:
        info = path.stat()
    except OSError as error:
        report["exists"] = False if error.errno == errno.ENOENT else None
        report["reason"] = error_kind(error)
        return report
    report.update(exists=True, mode=oct(stat.S_IMODE(info.st_mode)),
                  readable=os.access(path, os.R_OK), writable=os.access(path, os.W_OK))
    if not stat.S_ISCHR(info.st_mode):
        report["api_query_status"] = "not_character_device"
        return report
    descriptor = None
    try:
        import fcntl
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0))
        report["api_version"] = fcntl.ioctl(descriptor, KVM_GET_API_VERSION)
        report["api_query_status"] = "ok"
    except ImportError:
        report["api_query_status"] = "unavailable"
    except OSError as error:
        report["api_query_status"] = error_kind(error)
    finally:
        if descriptor is not None:
            os.close(descriptor)
    return report


def storage_report(path):
    try:
        result = os.statvfs(path)
        return {"status": "ok", "available_bytes": result.f_bavail * result.f_frsize,
                "total_bytes": result.f_blocks * result.f_frsize}
    except OSError as error:
        return {"status": "unavailable", "reason": error_kind(error), "available_bytes": None, "total_bytes": None}


def run_command(name, arguments, timeout=COMMAND_TIMEOUT, limit=COMMAND_LIMIT):
    """Capture a bounded amount in memory; never include diagnostics in the report."""
    executable = shutil.which(name)
    if not executable:
        return {"status": "unavailable", "returncode": None, "stdout": ""}
    environment = dict(os.environ, LC_ALL="C", LANG="C", MESA_SHADER_CACHE_DISABLE="true",
                       __GL_SHADER_DISK_CACHE="0")
    try:
        process = subprocess.Popen([executable] + arguments, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                   env=environment, start_new_session=True)
    except OSError:
        return {"status": "execution_error", "returncode": None, "stdout": ""}
    deadline = time.monotonic() + min(timeout, COMMAND_TIMEOUT)
    chunks, size, status = [], 0, "ok"
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    status = "timeout"
                    break
                for key, _ in selector.select(remaining):
                    data = os.read(key.fd, min(65536, limit + 1 - size))
                    if not data:
                        selector.unregister(key.fileobj)
                        continue
                    chunks.append(data)
                    size += len(data)
                    if size > limit:
                        status = "output_limit"
                        break
                if status != "ok":
                    break
        if status == "ok":
            try:
                process.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                status = "timeout"
    finally:
        if process.poll() is None or status != "ok":
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        process.stdout.close()
    if status == "ok" and process.returncode:
        status = "command_failed"
    return {"status": status, "returncode": process.returncode,
            "stdout": b"".join(chunks).decode("utf-8", errors="replace") if status == "ok" else ""}


def command_status(result):
    return {key: result[key] for key in ("status", "returncode")}


def parse_qemu_help(text, heading, choices):
    if heading.lower() not in text.lower():
        return dict.fromkeys(choices)
    lines = {line.strip().split()[0] for line in text.splitlines() if line.strip()}
    return {choice: choice in lines for choice in choices}


def collect_qemu(runner=run_command):
    report = {}
    version = runner("qemu-system-aarch64", ["--version"])
    match = re.search(r"QEMU emulator version (\d+(?:\.\d+)+)", version["stdout"])
    report["version"] = dict(command_status(version), value=match[1] if match else None)
    for key, args, heading, choices in (
        ("accelerators", ["-accel", "help"], "Accelerators supported", ("tcg", "kvm", "hvf")),
        ("cpus", ["-cpu", "help"], "Available CPUs", ("cortex-a57", "cortex-a76", "cortex-a710", "max", "host")),
        ("displays", ["-display", "help"], "Available display", ("gtk", "sdl", "egl-headless", "none", "curses", "cocoa", "dbus")),
    ):
        result = runner("qemu-system-aarch64", args)
        report[key] = dict(command_status(result), listed=parse_qemu_help(result["stdout"], heading, choices))
    return report


def parse_vulkaninfo(text):
    devices, current = [], None
    for line in text.splitlines():
        gpu = re.fullmatch(r"\s*GPU(\d+)\s*:\s*", line)
        if gpu:
            current = {"index": int(gpu[1]), "properties": {}, "features": dict.fromkeys(VK_FEATURES),
                       "extensions_listed": dict.fromkeys(VK_EXTENSIONS), "host_memory_flags_observed": [],
                       "minImportedHostPointerAlignment": None}
            devices.append(current)
            continue
        if current is None:
            continue
        key, separator, value = line.strip().partition("=")
        key, value = key.strip(), value.strip()
        extension = re.fullmatch(r"\s*(VK_\w+)\s*:\s*extension revision\s+\d+\s*", line)
        if extension and extension[1] in VK_EXTENSIONS:
            current["extensions_listed"][extension[1]] = True
        elif key in VK_EXTENSIONS and re.fullmatch(r"\d+", value):
            current["extensions_listed"][key] = True
        elif separator and key in VK_FEATURES and value.lower() in ("true", "false"):
            current["features"][key] = value.lower() == "true"
        elif separator and key in ("apiVersion", "driverVersion"):
            version = re.match(r"\d+(?:\.\d+)*", value)
            if version:
                current["properties"][key] = version[0]
        elif separator and key in ("vendorID", "deviceID") and re.fullmatch(r"(?:0x[0-9a-fA-F]+|\d+)", value):
            current["properties"][key] = value
        elif separator and key in ("deviceName", "deviceType", "driverName", "driverInfo", "driverID"):
            current["properties"][key] = safe_label(value)
        elif separator and key == "minImportedHostPointerAlignment" and value.isdigit():
            current[key] = int(value)
        flag = re.fullmatch(r"\s*(?:VK_)?MEMORY_PROPERTY_(HOST_VISIBLE|HOST_COHERENT|HOST_CACHED)_BIT\s*", line)
        if flag and flag[1] not in current["host_memory_flags_observed"]:
            current["host_memory_flags_observed"].append(flag[1])
    return [device for device in devices if device["properties"]]


def parse_glxinfo(text):
    allowed = {"OpenGL vendor string": "vendor", "OpenGL renderer string": "renderer",
               "OpenGL version string": "api_version", "OpenGL core profile version string": "core_api_version",
               "OpenGL ES profile version string": "es_api_version"}
    result = {}
    for line in text.splitlines():
        key, separator, value = line.strip().partition(":")
        if separator and key in allowed:
            result[allowed[key]] = safe_label(value)
    return result


def collect_graphics(runner=run_command):
    vulkan = runner("vulkaninfo", [])
    gl = runner("glxinfo", ["-B"])
    devices, fields = parse_vulkaninfo(vulkan["stdout"]), parse_glxinfo(gl["stdout"])
    return {"vulkan": dict(command_status(vulkan), devices=devices or None),
            "opengl": dict(command_status(gl), properties=fields or None),
            "render_test_performed": False, "host_memory_import_tested": False}


def collect(storage_path, graphics=False, root=Path("/"), runner=run_command, identity=None):
    system, machine, kernel = identity or (platform.system(), platform.machine(), platform.release())
    report = {"schema_version": 1, "system": system, "machine": machine, "kernel": kernel,
              "null_means": "not observed; not proof that a feature is unsupported",
              "boot_test_performed": False, "storage": storage_report(storage_path)}
    for key, filename, parser in (("os_release", "etc/os-release", parse_os_release),
                                  ("cpu", "proc/cpuinfo", parse_cpuinfo),
                                  ("memory", "proc/meminfo", parse_meminfo),
                                  ("swap", "proc/swaps", parse_swaps)):
        data, status = read_text(root / filename)
        report[key] = {"status": status, "data": parser(data) if data is not None else None}
    device_tree = {}
    for name in ("model", "compatible"):
        data, status = read_text(root / "sys/firmware/devicetree/base" / name)
        if data is None:
            data, status = read_text(root / "proc/device-tree" / name)
        values = [safe_label(value) for value in (data or "").split("\0") if value]
        device_tree[name] = {"status": status, "values": values if data is not None else None}
    report.update(device_tree=device_tree, zram=collect_zram(root), kernel_config=collect_config(root, kernel),
                  kvm=collect_kvm(root), qemu=collect_qemu(runner),
                  graphics=collect_graphics(runner) if graphics else {"status": "not_requested"})
    return report


def write_report(report, output=None):
    data = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if output is None:
        sys.stdout.write(data)
    else:
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--storage-path", type=Path, default=Path.cwd(), help="existing filesystem location to measure; path is omitted from JSON")
    parser.add_argument("--output", type=Path, help="create a new report file; never overwrite")
    parser.add_argument("--graphics", action="store_true", help="also query installed vulkaninfo and glxinfo; each has a 15-second timeout")
    args = parser.parse_args(argv)
    if args.output is not None and os.path.lexists(args.output):
        parser.error("output already exists; choose a new filename")
    try:
        write_report(collect(args.storage_path, args.graphics), args.output)
    except OSError as error:
        parser.error("could not create report: " + error_kind(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
