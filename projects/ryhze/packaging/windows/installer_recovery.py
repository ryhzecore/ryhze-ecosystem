#!/usr/bin/env python3
"""Inspect an owned installation and retain/restore an inert recovery payload.

Never installs, activates, runs, renames, deletes, repairs in place or edits the
registry. Production authentication and the application/job shutdown handshake
remain prerequisites of a future activation adapter, not facts supplied by JSON.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
from typing import Iterator

from prepare_bundle import _safe_component

MAX_FILES = 20000
MAX_TOTAL_BYTES = 2 * 1024 * 1024 * 1024
MAX_METADATA_BYTES = 8 * 1024 * 1024
_HASH = re.compile(r"[a-f0-9]{64}\Z")
_VERSION = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:\+(0|[1-9]\d*))?\Z")
_OWNER = re.compile(r"\{[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\}\Z")
PRODUCTS = {
    "RyhzeEntertainment": ("Ryhze-Entertainment", r"Software\Ryhze\Entertainment", "ryhze"),
    "RyhzeInstallerAcceptance": ("Ryhze-Installer-Acceptance", r"Software\Ryhze\InstallerAcceptance", "ryhze-installer-acceptance"),
}


def version(value: str) -> tuple[int, int, int, int]:
    match = _VERSION.fullmatch(value) if isinstance(value, str) else None
    if not match:
        raise ValueError("A canonical numeric version is required")
    parts = tuple(int(part or 0) for part in match.groups())
    if any(part > 2147483647 for part in parts):
        raise ValueError("Version component is out of bounds")
    return parts


def relative(value: str) -> Path:
    if not isinstance(value, str) or len(value) > 4096 or "\\" in value:
        raise ValueError("Invalid manifest path")
    parts = value.split("/")
    if not parts or not all(_safe_component(part) for part in parts):
        raise ValueError("Unsafe manifest path")
    if str(PurePosixPath(value)) != value:
        raise ValueError("Noncanonical manifest path")
    return Path(*parts)


def records(value: object) -> list[dict]:
    if not isinstance(value, list) or not 1 <= len(value) <= MAX_FILES:
        raise ValueError("Bounded nonempty file inventory is required")
    seen: set[str] = set()
    total = 0
    result = []
    for item in value:
        if not isinstance(item, dict) or set(item) != {"path", "bytes", "sha256"}:
            raise ValueError("Invalid file inventory schema")
        path = relative(item["path"])
        key = str(path).casefold()
        if key in seen:
            raise ValueError("Duplicate/case-colliding file identity")
        seen.add(key)
        count = item["bytes"]
        if type(count) is not int or count < 0 or not isinstance(item["sha256"], str) or not _HASH.fullmatch(item["sha256"]):
            raise ValueError("Invalid file length or digest")
        total += count
        if total > MAX_TOTAL_BYTES:
            raise ValueError("Payload exceeds recovery budget")
        result.append(dict(item))
    if "ryhze.exe" not in seen:
        raise ValueError("The owned application executable is required")
    # A file cannot also be another record's ancestor.
    for item in result:
        if any(str(parent).casefold() in seen for parent in relative(item["path"]).parents if parent != Path('.')):
            raise ValueError("File/directory identity collision")
    return result


def decode(data: bytes) -> dict:
    if len(data) > MAX_METADATA_BYTES:
        raise ValueError("Metadata exceeds budget")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate metadata key")
            result[key] = value
        return result
    value = json.loads(data.decode("utf-8"), object_pairs_hook=unique)
    if not isinstance(value, dict):
        raise ValueError("Metadata object required")
    return value


def source_manifest(data: bytes, expected_sha256: str) -> dict:
    if not _HASH.fullmatch(expected_sha256) or hashlib.sha256(data).hexdigest() != expected_sha256:
        raise ValueError("Source manifest identity mismatch")
    manifest = decode(data)
    required = {"format", "product", "version", "source_revision", "artifact_kind", "dependency_lock_sha256", "files"}
    if set(manifest) != required or type(manifest["format"]) is not int or manifest["format"] != 1 or manifest["product"] != "Ryhze":
        raise ValueError("Unsupported application manifest")
    if manifest["artifact_kind"] != "unsigned-application-payload":
        raise ValueError("This local recovery tool does not infer another artifact policy")
    version(manifest["version"])
    if not re.fullmatch(r"[a-f0-9]{40}", manifest["source_revision"]) or not _HASH.fullmatch(manifest["dependency_lock_sha256"]):
        raise ValueError("Invalid provenance identity")
    manifest["files"] = records(manifest["files"])
    return manifest


@contextmanager
def pinned(path: Path, *, directory: bool = False) -> Iterator[object]:
    """Read handle with denied write/delete sharing on Windows; fixture fallback elsewhere."""
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 1024:
        raise ValueError("Reparse/link path refused")
    if directory != stat.S_ISDIR(info.st_mode) or (not directory and not stat.S_ISREG(info.st_mode)):
        raise ValueError("Unexpected filesystem object")
    if not directory and info.st_nlink != 1:
        raise ValueError("Hard-linked file refused")
    if os.name != "nt":
        if directory:
            yield None
        else:
            with path.open("rb") as stream:
                yield stream
        return
    import msvcrt
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    access = 0xA0 if directory else 0x80000000  # READ_ATTRIBUTES/TRAVERSE or GENERIC_READ
    handle = kernel.CreateFileW(str(path), access, 1, None, 3, 0x02200000, None)
    if handle == wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), "Cannot pin recovery input")
    try:
        # Check the opened object rather than trusting the preceding path lstat.
        class AttributeTag(ctypes.Structure):
            _fields_ = [("attributes", wintypes.DWORD), ("tag", wintypes.DWORD)]
        kernel.GetFileInformationByHandleEx.argtypes = (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
        tag = AttributeTag()
        if (not kernel.GetFileInformationByHandleEx(handle, 9, ctypes.byref(tag), ctypes.sizeof(tag))
                or tag.attributes & 1024 or bool(tag.attributes & 16) != directory):
            raise ValueError("Opened recovery input is not a verified regular path")
        if directory:
            yield None
        else:
            descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
            handle = None  # transferred to the CRT stream
            with os.fdopen(descriptor, "rb") as stream:
                opened = os.fstat(stream.fileno())
                if opened.st_nlink != 1 or (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
                    raise ValueError("Recovery input identity changed")
                yield stream
    finally:
        if handle is not None:
            kernel.CloseHandle(handle)


@contextmanager
def pinned_ancestors(path: Path) -> Iterator[None]:
    from contextlib import ExitStack
    absolute = path.absolute()
    if ".." in absolute.parts:
        raise ValueError("Parent traversal is not a local authority path")
    if os.name == "nt" and (not re.match(r"^[A-Za-z]:\\", str(absolute)) or str(absolute).startswith("\\")):
        raise ValueError("A local absolute path is required")
    if os.name == "nt":
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetDriveTypeW.argtypes = (wintypes.LPCWSTR,)
        if kernel.GetDriveTypeW(absolute.anchor) not in (2, 3):
            raise ValueError("A fixed or removable local drive is required")
    with ExitStack() as stack:
        for parent in reversed((absolute, *absolute.parents)):
            stack.enter_context(pinned(parent, directory=True))
        yield


def digest(stream, expected: dict, destination=None) -> None:
    value = hashlib.sha256()
    total = 0
    while True:
        chunk = stream.read(65536)
        if not chunk:
            break
        total += len(chunk)
        if total > expected["bytes"]:
            raise ValueError("Owned payload grew beyond its manifest")
        value.update(chunk)
        if destination is not None:
            destination.write(chunk)
    if total != expected["bytes"] or value.hexdigest() != expected["sha256"]:
        raise ValueError("Owned payload does not match its manifest")


def validate_owned(root: Path, manifest: dict, observation: dict, *, target_version: str, mode: str) -> dict:
    if mode not in ("upgrade", "repair"):
        raise ValueError("Explicit upgrade or repair intent required")
    product = observation.get("product")
    if product not in PRODUCTS or root.name != PRODUCTS[product][0]:
        raise ValueError("A matching dedicated same-product root is required")
    owner = observation.get("owner_id")
    if not isinstance(owner, str) or not _OWNER.fullmatch(owner):
        raise ValueError("Invalid existing installation owner")
    if observation.get("install_root") != str(root.absolute()) or observation.get("registered_owner") != owner:
        raise ValueError("Existing installation registration/marker mismatch")
    current = version(manifest["version"])
    installed = version(observation.get("installed_version", ""))
    # NSIS display metadata omits the build suffix; the exact manifest keeps it.
    if installed[:3] != current[:3] or ("+" in observation["installed_version"] and installed != current):
        raise ValueError("Installed version and verified manifest disagree")
    if observation.get("binary_version") is not None and version(observation["binary_version"]) != current:
        raise ValueError("Installed executable build and verified manifest disagree")
    target = version(target_version)
    if (mode == "upgrade" and target <= current) or (mode == "repair" and target != current):
        raise ValueError("Upgrade must advance version; repair must preserve exact version")
    if observation.get("running") is not False:
        raise ValueError("Installation must be observed stopped before retention")
    if observation.get("registration_verified") is not True:
        raise ValueError("Complete existing registration verification required")
    with pinned_ancestors(root):
        for record in manifest["files"]:
            path = root / "app" / relative(record["path"])
            with pinned_ancestors(path.parent), pinned(path) as stream:
                digest(stream, record)
    return {"product": product, "owner_id": owner, "install_root": str(root.absolute()),
            "version": manifest["version"], "target_version": target_version, "mode": mode,
            "files": manifest["files"], "authentication": "local-owner-selected-unsigned-provenance",
            "activation_allowed": False}


def _new_destination(destination: Path, forbidden_root: Path) -> None:
    destination = destination.absolute()
    if ".." in destination.parts or ".." in forbidden_root.parts:
        raise ValueError("Parent traversal is not a recovery destination")
    if destination == forbidden_root.absolute() or forbidden_root.absolute() in destination.parents:
        raise ValueError("Recovery output must be outside the installation/input root")
    if destination.exists() or destination.is_symlink():
        raise ValueError("Recovery output must be new; existing evidence is preserved")
    # Parent is required rather than constructing an unchecked arbitrary tree.
    if not destination.parent.is_dir():
        raise ValueError("Recovery output parent must already exist")


@contextmanager
def new_output(destination: Path, name: Path):
    """Create parents one component at a time while holding their namespace pins."""
    from contextlib import ExitStack
    with ExitStack() as stack:
        stack.enter_context(pinned(destination, directory=True))
        parent = destination
        for part in name.parts[:-1]:
            parent = parent / part
            parent.mkdir(exist_ok=True)
            stack.enter_context(pinned(parent, directory=True))
        path = parent / name.name
        if os.name != "nt":
            with path.open("xb") as stream:
                yield stream
            return
        import msvcrt
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel.CreateFileW(str(path), 0x40000000, 0, None, 1, 0x00200000, None)
        if handle == wintypes.HANDLE(-1).value:
            raise OSError(ctypes.get_last_error(), "Cannot exclusively create recovery output")
        try:
            descriptor = msvcrt.open_osfhandle(handle, os.O_WRONLY | os.O_BINARY)
            handle = None
            with os.fdopen(descriptor, "wb") as stream:
                yield stream
        finally:
            if handle is not None:
                kernel.CloseHandle(handle)


def retain(root: Path, destination: Path, plan: dict, manifest_sha256: str, *, reader_min: int, reader_max: int) -> str:
    if type(reader_min) is not int or type(reader_max) is not int or not 0 <= reader_min <= reader_max <= 2147483647:
        raise ValueError("Explicit bounded reader schema compatibility required")
    if not _HASH.fullmatch(manifest_sha256) or plan.get("activation_allowed") is not False or plan.get("install_root") != str(root.absolute()):
        raise ValueError("Verified inert preflight plan required")
    inventory = records(plan["files"])
    _new_destination(destination, root)
    with pinned_ancestors(root), pinned_ancestors(destination.parent):
        destination.mkdir()
        for record in inventory:
            source = root / "app" / relative(record["path"])
            with pinned_ancestors(source.parent), pinned(source) as stream, new_output(destination, Path("app") / relative(record["path"])) as output:
                digest(stream, record, output)
                output.flush()
                os.fsync(output.fileno())
        metadata = {"format": 1, "kind": "inert-owned-payload-recovery", "product": plan["product"],
                    "owner_id": plan["owner_id"], "install_root": plan["install_root"],
                    "version": plan["version"], "source_manifest_sha256": manifest_sha256,
                    "reader_schema_min": reader_min, "reader_schema_max": reader_max,
                    "files": inventory, "activation_allowed": False}
        data = (json.dumps(metadata, sort_keys=True, indent=2) + "\n").encode()
        # Completion metadata is committed last. A failed copy remains incomplete.
        with new_output(destination, Path("recovery.json")) as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
    return hashlib.sha256(data).hexdigest()


def restore(recovery: Path, destination: Path, *, expected_sha256: str, store_schema: int) -> dict:
    _new_destination(destination, recovery)
    with pinned_ancestors(recovery), pinned(recovery / "recovery.json") as stream:
        data = stream.read(MAX_METADATA_BYTES + 1)
        if not _HASH.fullmatch(expected_sha256) or hashlib.sha256(data).hexdigest() != expected_sha256:
            raise ValueError("Recovery metadata identity mismatch")
        metadata = decode(data)
        required = {"format", "kind", "product", "owner_id", "install_root", "version", "source_manifest_sha256",
                    "reader_schema_min", "reader_schema_max", "files", "activation_allowed"}
        if set(metadata) != required or type(metadata["format"]) is not int or metadata["format"] != 1 or metadata["kind"] != "inert-owned-payload-recovery" or metadata["activation_allowed"] is not False:
            raise ValueError("Unsupported recovery record")
        if metadata["product"] not in PRODUCTS or not _OWNER.fullmatch(metadata["owner_id"]) or not _HASH.fullmatch(metadata["source_manifest_sha256"]):
            raise ValueError("Invalid recovery ownership")
        version(metadata["version"])
        lower, upper = metadata["reader_schema_min"], metadata["reader_schema_max"]
        if type(store_schema) is not int or type(lower) is not int or type(upper) is not int or not 0 <= lower <= store_schema <= upper <= 2147483647:
            raise ValueError("Older payload cannot read this store schema")
        inventory = records(metadata["files"])
        # Verify all source files before producing any restored payload.
        for record in inventory:
            source = recovery / "app" / relative(record["path"])
            with pinned_ancestors(source.parent), pinned(source) as item:
                digest(item, record)
        with pinned_ancestors(destination.parent):
            destination.mkdir()
            for record in inventory:
                source = recovery / "app" / relative(record["path"])
                with pinned_ancestors(source.parent), pinned(source) as item, new_output(destination, Path("app") / relative(record["path"])) as output:
                    digest(item, record, output)
                    output.flush()
                    os.fsync(output.fileno())
    return {"restored_version": metadata["version"], "store_schema": store_schema,
            "destination": str(destination.absolute()), "activation_allowed": False,
            "existing_installation_changed": False, "user_data_changed": False}


def windows_observation(root: Path, product: str) -> dict:
    """Live read-only boundary, never used by portable synthetic unit fixtures."""
    if os.name != "nt" or product not in PRODUCTS:
        raise ValueError("Windows and an explicit supported product identity are required")
    import configparser
    import winreg
    from ctypes import wintypes
    owner_marker = configparser.ConfigParser(interpolation=None, strict=True)
    with pinned_ancestors(root), pinned(root / ".ryhze-install.ini") as stream:
        raw = stream.read(65537)
        if len(raw) > 65536:
            raise ValueError("Ownership marker exceeds budget")
        # NSIS WriteINIStr uses the system ANSI encoding unless a BOM exists.
        text = (raw.decode("utf-16") if raw.startswith((b"\xff\xfe", b"\xfe\xff"))
                else raw.decode("utf-8-sig") if raw.startswith(b"\xef\xbb\xbf") else raw.decode("mbcs"))
        owner_marker.read_string(text)
    marker = owner_marker["Install"]
    if marker["Product"] != product or os.path.normcase(marker["Root"]) != os.path.normcase(str(root.absolute())):
        raise ValueError("Ownership marker belongs to another product/root")
    def read(key, name):
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key, 0, winreg.KEY_READ | winreg.KEY_WOW64_32KEY) as opened:
            value, kind = winreg.QueryValueEx(opened, name)
            if kind != winreg.REG_SZ:
                raise ValueError("Unexpected registration value type")
            return value
    _, product_key, scheme = PRODUCTS[product]
    uninstall = rf"Software\Microsoft\Windows\CurrentVersion\Uninstall\{product}"
    protocol = rf"Software\Classes\{scheme}"
    owner = marker["OwnerId"]
    registered_owner = read(product_key, "RyhzeOwnerId")
    expected = ((product_key, "InstallDir", str(root.absolute())),
                (uninstall, "RyhzeOwnerId", owner), (uninstall, "InstallLocation", str(root.absolute())),
                (uninstall, "UninstallString", f'"{root.absolute()}\\Uninstall.exe"'),
                (protocol, "RyhzeOwnerId", owner), (protocol, "RyhzeInstallRoot", str(root.absolute())),
                (protocol + r"\shell\open\command", "", f'"{root.absolute()}\\app\\ryhze.exe" "%1"'))
    if any(read(key, name) != expected_value for key, name, expected_value in expected):
        raise ValueError("Existing registration has changed owner or command")
    version_api = ctypes.WinDLL("version", use_last_error=True)
    version_api.GetFileVersionInfoSizeW.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD))
    version_api.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    version_api.GetFileVersionInfoW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p)
    version_api.VerQueryValueW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT))
    ignored = wintypes.DWORD()
    executable = root.absolute() / "app" / "ryhze.exe"
    version_bytes = version_api.GetFileVersionInfoSizeW(str(executable), ctypes.byref(ignored))
    binary_version = None
    if version_bytes:
        if version_bytes > 1024 * 1024:
            raise ValueError("Executable version resource exceeds budget")
        buffer = ctypes.create_string_buffer(version_bytes)
        pointer = ctypes.c_void_p()
        length = wintypes.UINT()
        if (not version_api.GetFileVersionInfoW(str(executable), 0, version_bytes, buffer)
                or not version_api.VerQueryValueW(buffer, "\\", ctypes.byref(pointer), ctypes.byref(length))
                or length.value < 13 * ctypes.sizeof(wintypes.DWORD)):
            raise OSError("Cannot inspect installed executable version")
        fixed = ctypes.cast(pointer, ctypes.POINTER(wintypes.DWORD * 13)).contents
        if fixed[0] != 0xFEEF04BD:
            raise ValueError("Invalid executable version resource")
        major, minor, patch, build = fixed[2] >> 16, fixed[2] & 0xffff, fixed[3] >> 16, fixed[3] & 0xffff
        binary_version = f"{major}.{minor}.{patch}+{build}"
    elif ctypes.get_last_error() not in (1812, 1813, 1814, 1815):
        raise OSError("Cannot inspect executable version availability")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    class ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("pid", wintypes.DWORD),
                    ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
                    ("exe", wintypes.WCHAR * 260)]
    kernel.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.Process32FirstW.argtypes = (wintypes.HANDLE, ctypes.POINTER(ProcessEntry))
    kernel.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(ProcessEntry))
    kernel.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == wintypes.HANDLE(-1).value:
        raise OSError("Cannot observe running processes")
    running = False
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        more = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            if entry.exe.casefold() == "ryhze.exe":
                process = kernel.OpenProcess(0x1000, False, entry.pid)
                if not process:
                    raise OSError("Cannot exclude a running Ryhze process")
                try:
                    size = wintypes.DWORD(32768)
                    image = ctypes.create_unicode_buffer(size.value)
                    if not kernel.QueryFullProcessImageNameW(process, 0, image, ctypes.byref(size)):
                        raise OSError("Cannot identify running Ryhze path")
                    running |= os.path.normcase(image.value) == os.path.normcase(str(root.absolute() / "app" / "ryhze.exe"))
                finally:
                    kernel.CloseHandle(process)
            more = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        if ctypes.get_last_error() != 18:
            raise OSError("Incomplete process observation")
    finally:
        kernel.CloseHandle(snapshot)
    return {"product": product, "owner_id": owner, "install_root": str(root.absolute()),
            "registered_owner": registered_owner, "installed_version": read(uninstall, "DisplayVersion"),
            "binary_version": binary_version,
            "running": running, "registration_verified": True}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    task = commands.add_parser("validate-manifest")
    task.add_argument("--manifest", type=Path, required=True)
    task.add_argument("--manifest-sha256", required=True)
    for command in ("inspect", "retain"):
        task = commands.add_parser(command)
        task.add_argument("--root", type=Path, required=True)
        task.add_argument("--product", choices=PRODUCTS, required=True)
        task.add_argument("--manifest", type=Path, required=True)
        task.add_argument("--manifest-sha256", required=True)
        task.add_argument("--target-version", required=True)
        task.add_argument("--mode", choices=("upgrade", "repair"), required=True)
        if command == "retain":
            task.add_argument("--output", type=Path, required=True)
            task.add_argument("--reader-schema-min", type=int, required=True)
            task.add_argument("--reader-schema-max", type=int, required=True)
    task = commands.add_parser("restore")
    task.add_argument("--recovery", type=Path, required=True)
    task.add_argument("--recovery-sha256", required=True)
    task.add_argument("--output", type=Path, required=True)
    task.add_argument("--store-schema", type=int, required=True)
    args = parser.parse_args()
    if args.command == "restore":
        result = restore(args.recovery, args.output, expected_sha256=args.recovery_sha256, store_schema=args.store_schema)
    else:
        with pinned_ancestors(args.manifest.absolute().parent), pinned(args.manifest.absolute()) as stream:
            manifest = source_manifest(stream.read(MAX_METADATA_BYTES + 1), args.manifest_sha256)
        if args.command == "validate-manifest":
            print(json.dumps({"version": manifest["version"], "source_revision": manifest["source_revision"],
                              "manifest_sha256": args.manifest_sha256, "file_count": len(manifest["files"]),
                              "payload_bytes": sum(item["bytes"] for item in manifest["files"]),
                              "activation_allowed": False}, sort_keys=True, indent=2))
            return
        plan = validate_owned(args.root.absolute(), manifest, windows_observation(args.root.absolute(), args.product),
                              target_version=args.target_version, mode=args.mode)
        result = plan
        if args.command == "retain":
            # Refresh volatile process/registration preconditions before copying.
            validate_owned(args.root.absolute(), manifest, windows_observation(args.root.absolute(), args.product),
                           target_version=args.target_version, mode=args.mode)
            result = {"recovery_sha256": retain(args.root.absolute(), args.output.absolute(), plan, args.manifest_sha256,
                       reader_min=args.reader_schema_min, reader_max=args.reader_schema_max), "activation_allowed": False}
    print(json.dumps(result, sort_keys=True, indent=2))


if __name__ == "__main__":
    main()
