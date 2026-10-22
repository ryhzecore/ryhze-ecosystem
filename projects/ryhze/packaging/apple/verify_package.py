"""Inspect an IPA or built iOS .app. This does not validate Apple signing."""

import argparse
import os
import plistlib
import re
import stat
import zipfile
from pathlib import Path, PurePosixPath

from check_privacy import verify as verify_privacy


FORBIDDEN_SUFFIXES = {
    ".py", ".pyc", ".ps1", ".sh", ".bash", ".bat", ".cmd", ".exe", ".dll",
    ".msi", ".msix", ".appimage", ".dylib", ".so",
}
EXECUTABLE_MAGIC = {
    b"\x7fELF", b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}


class DirectoryPackage:
    """Expose built app files using the same checks as an IPA, without following links."""

    def __init__(self, root):
        root = Path(root)
        if root.suffix != ".app":
            raise ValueError("Expected an iOS .app directory")
        self.prefix = f"Payload/{root.name}/"
        self.root = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        self.entries = []
        try:
            self._scan(self.root, "")
        except BaseException:
            os.close(self.root)
            raise

    def _scan(self, descriptor, parent):
        with os.scandir(descriptor) as children:
            for child in children:
                metadata = child.stat(follow_symlinks=False)
                if stat.S_ISLNK(metadata.st_mode):
                    raise ValueError("Symbolic link in built iOS application")
                name = parent + child.name
                if stat.S_ISDIR(metadata.st_mode):
                    nested = os.open(child.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                     dir_fd=descriptor)
                    try:
                        self._scan(nested, name + "/")
                    finally:
                        os.close(nested)
                elif stat.S_ISREG(metadata.st_mode):
                    entry = zipfile.ZipInfo(self.prefix + name)
                    entry.file_size = metadata.st_size
                    entry.external_attr = metadata.st_mode << 16
                    self.entries.append(entry)
                    if len(self.entries) > 100000:
                        raise ValueError("Application has too many entries")
                else:
                    raise ValueError("Nonregular file in built iOS application")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        os.close(self.root)

    def infolist(self):
        return self.entries

    def getinfo(self, name):
        for entry in self.entries:
            if entry.filename == name:
                return entry
        raise KeyError("Missing application file")

    def open(self, entry):
        # Resolve every component relative to an already opened directory. A
        # concurrent symlink substitution cannot redirect a content read outside it.
        parts = entry.filename.removeprefix(self.prefix).split("/")
        if any(part in {"", ".", ".."} for part in parts):
            raise ValueError("Unsafe application file path")
        descriptor = os.dup(self.root)
        try:
            for part in parts[:-1]:
                nested = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                 dir_fd=descriptor)
                os.close(descriptor)
                descriptor = nested
            file_descriptor = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW,
                                      dir_fd=descriptor)
            if not stat.S_ISREG(os.fstat(file_descriptor).st_mode):
                os.close(file_descriptor)
                raise ValueError("Application file is not regular")
            return os.fdopen(file_descriptor, "rb")
        finally:
            os.close(descriptor)

    def read(self, entry):
        with self.open(entry) as file:
            # Only the bounded Info.plist uses read(); assets use a four-byte read.
            value = file.read(1024 * 1024 + 1)
        if len(value) > 1024 * 1024:
            raise ValueError("Application metadata is too large")
        return value


def source_version():
    pubspec = Path(__file__).resolve().parents[2] / "pubspec.yaml"
    match = re.search(r"^version:\s*([0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?)\+([0-9]+)\s*$",
                      pubspec.read_text(), re.MULTILINE)
    if not match:
        raise ValueError("Expected an explicit product version and build in pubspec.yaml")
    return match.group(1), match.group(2)


def verify_package(path, *, bundle="com.ryhze.ryhze", version=None, build=None):
    source_name, source_build = source_version()
    version = source_name if version is None else version
    build = source_build if build is None else build
    path = Path(path)
    with (DirectoryPackage(path) if path.is_dir() else zipfile.ZipFile(path)) as package:
        entries = package.infolist()
        if len(entries) > 100000:
            raise ValueError("Package has too many entries")
        seen = set()
        info_entries = [entry for entry in entries
                        if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", entry.filename)]
        if len(info_entries) != 1:
            raise ValueError("Expected exactly one iOS application Info.plist")
        app_prefix = str(PurePosixPath(info_entries[0].filename).parent) + "/"
        asset_count = 0
        for entry in entries:
            name = entry.filename
            parts = name.rstrip("/").split("/")
            if (not name or name.startswith("/") or "\\" in name
                    or any(part in {"", ".", ".."} for part in parts)
                    or any(ord(c) < 32 for c in name)):
                raise ValueError("Unsafe package entry path")
            folded = name.rstrip("/").casefold()
            if folded in seen:
                raise ValueError("Duplicate package entry")
            seen.add(folded)
            if "flutter_assets" not in [part.casefold() for part in parts]:
                continue
            if not name.startswith(app_prefix):
                raise ValueError("Flutter assets are outside the main application")
            if entry.is_dir():
                continue
            asset_count += 1
            if stat.S_ISLNK(entry.external_attr >> 16):
                raise ValueError("Symbolic link in Flutter assets")
            if ("/assets/update/" in folded or "/assets/updater/" in folded
                    or PurePosixPath(folded).suffix in FORBIDDEN_SUFFIXES):
                raise ValueError("Desktop updater or executable asset in IPA")
            with package.open(entry) as asset:
                magic = asset.read(4)
            if magic in EXECUTABLE_MAGIC or magic[:2] in {b"MZ", b"#!"}:
                raise ValueError("Executable content in Flutter assets")
        if not asset_count:
            raise ValueError("Missing Flutter assets")
        entry = info_entries[0]
        if entry.file_size > 1024 * 1024:
            raise ValueError("Application Info.plist is too large")
        info = plistlib.loads(package.read(entry))
        try:
            privacy = package.getinfo(app_prefix + "PrivacyInfo.xcprivacy")
        except KeyError as error:
            raise ValueError("Missing application privacy manifest") from error
        if privacy.file_size > 1024 * 1024:
            raise ValueError("Application privacy manifest is too large")
        verify_privacy(plistlib.loads(package.read(privacy)))
        expected = {
            "CFBundleIdentifier": bundle,
            "CFBundleShortVersionString": version,
            "CFBundleVersion": build,
            "CFBundlePackageType": "APPL",
        }
        for key, value in expected.items():
            if info.get(key) != value:
                raise ValueError(f"Unexpected {key}")
        executable = info.get("CFBundleExecutable")
        if (not isinstance(executable, str) or not executable
                or "/" in executable or "\\" in executable
                or executable in {".", ".."}):
            raise ValueError("Invalid application executable name")
        executable_path = str(PurePosixPath(entry.filename).parent / executable)
        try:
            binary = package.getinfo(executable_path)
        except KeyError as error:
            raise ValueError("Application executable is missing") from error
        if stat.S_ISLNK(binary.external_attr >> 16):
            raise ValueError("Application executable is a symbolic link")
        if binary.file_size == 0:
            raise ValueError("Application executable is empty")
    return {"bundle": bundle, "version": version, "build": build,
            "flutter_assets": asset_count}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", help="IPA archive or compiled iOS .app directory")
    parser.add_argument("--version", help="Approved product version; defaults to current pubspec")
    parser.add_argument("--build", help="Verified build number; defaults to current pubspec")
    arguments = parser.parse_args()
    verify_package(arguments.package, version=arguments.version, build=arguments.build)
    print("iOS package asset and metadata checks passed; signing and Apple acceptance unverified")
