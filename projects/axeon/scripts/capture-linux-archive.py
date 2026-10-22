#!/usr/bin/env python3
"""Validate a committed Git TAR; optionally capture its exact bytes on ext4.

--validate-only is read-only and works on Windows. Native extraction must run
under the existing host/native admission guard with AXEON_RUN_DIR set. It never
starts WSL, changes a checkout, reuses a destination, or builds/tests anything.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tarfile

BASE = Path("/home/Leo/.cache/axeon/build-snapshots")
EVIDENCE = Path("/mnt/c/Projects/Axeon/Evidence")
MAX_ARCHIVE = 512 << 20
MAX_FILE = 512 << 20
MAX_TOTAL = 768 << 20
MAX_ENTRIES = 16384
MIN_DISK = 4 << 30


class ResourceHold(RuntimeError):
    pass


def need(value, message):
    if not value:
        raise RuntimeError(message)


def plain(value):
    path = Path(os.path.abspath(value))
    for ancestor in reversed((path, *path.parents)):
        try:
            info = ancestor.lstat()
        except FileNotFoundError:
            continue
        need(not stat.S_ISLNK(info.st_mode) and not getattr(info, "st_file_attributes", 0) & 0x400,
             f"redirected path is forbidden: {ancestor}")
    return path


def regular(value, maximum):
    path = plain(value)
    info = path.stat()
    need(stat.S_ISREG(info.st_mode) and info.st_size <= maximum, f"bounded regular file required: {path}")
    return path


def identity(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns


def digest(value, maximum=MAX_FILE):
    path = regular(value, maximum)
    before = path.stat()
    result, count = hashlib.sha256(), 0
    with path.open("rb") as stream:
        need(identity(os.fstat(stream.fileno())) == identity(before), f"input replaced: {path}")
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            count += len(chunk)
            need(count <= maximum, f"input grew beyond budget: {path}")
            result.update(chunk)
        after = os.fstat(stream.fileno())
    need(identity(before) == identity(after) == identity(plain(path).stat()) and count == before.st_size,
         f"input changed while reading: {path}")
    return result.hexdigest()


def read_json(value):
    def pairs(items):
        result = {}
        for key, item in items:
            need(key not in result, f"duplicate JSON key: {key}")
            result[key] = item
        return result
    def invalid(value):
        raise RuntimeError(f"nonfinite JSON value: {value}")
    path = regular(value, 16 << 20)
    before = digest(path)
    result = json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=pairs, parse_constant=invalid)
    need(digest(path) == before, "JSON input changed while parsing")
    return result, before


def relative(value):
    need(isinstance(value, str) and value and not value.startswith("/") and "\\" not in value
         and ":" not in value and not any(ord(c) < 32 or ord(c) == 127 for c in value), "unsafe archive path")
    value.encode("utf-8", errors="strict")
    parts = value.split("/")
    need(all(part not in ("", ".", "..") and not part.endswith((".", " "))
             and not re.fullmatch(r"(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", part)
             for part in parts), f"unsafe path component: {value}")
    need(not any(part.casefold() == ".git" for part in parts)
         and parts[0].casefold() not in ("target", ".snapshot-manifest.json"), "protected snapshot path in archive")
    return value


def register(names, name, kind):
    """Check ancestor spelling and file/directory conflicts as well as leaves."""
    parts = name.split("/")
    for index in range(1, len(parts) + 1):
        prefix = "/".join(parts[:index])
        wanted = kind if index == len(parts) else "directory"
        prior = names.get(prefix.casefold())
        need(prior is None or prior == (prefix, wanted), f"case alias or file/directory collision: {prefix}")
        names[prefix.casefold()] = (prefix, wanted)


def expected_inputs(args):
    need(re.fullmatch(r"[0-9a-f]{40}", args.source_commit), "full lowercase Git commit required")
    need(re.fullmatch(r"[0-9a-f]{64}", args.archive_sha256), "lowercase archive SHA-256 required")
    manifest, manifest_sha = read_json(args.source_manifest)
    need(manifest.get("commit", manifest.get("source_commit")) == args.source_commit
         and manifest.get("archive_sha256") == args.archive_sha256, "source manifest commit/archive differs")
    entries = manifest.get("files")
    need(isinstance(entries, list) and 0 < len(entries) <= MAX_ENTRIES, "invalid source manifest inventory")
    files, names, total = {}, {}, 0
    for item in entries:
        name = relative(item["path"])
        need(name not in files, "duplicate source manifest entry")
        register(names, name, "file")
        need(re.fullmatch(r"[0-9a-f]{64}", item.get("sha256", "")), "invalid source file SHA-256")
        need(type(item.get("bytes")) is int and 0 <= item["bytes"] <= MAX_FILE, "source file size required")
        total += item["bytes"]
        need(total <= MAX_TOTAL, "source payload byte budget exceeded")
        files[name] = {"path": name, "sha256": item["sha256"], "bytes": item["bytes"], "mode": item.get("mode")}
    mode_sha = None
    if args.mode_manifest is not None:
        modes, mode_sha = read_json(args.mode_manifest)
        need(modes.get("source_commit") == args.source_commit and modes.get("source_manifest_sha256") == manifest_sha,
             "Git mode manifest is not bound to this source manifest/commit")
        mode_entries = modes.get("files")
        need(isinstance(mode_entries, list) and len(mode_entries) == len(files), "Git mode inventory differs")
        seen = set()
        for item in mode_entries:
            name = relative(item["path"])
            need(name in files and name not in seen and item.get("mode") in ("100644", "100755"), "invalid Git mode entry")
            need(files[name]["mode"] in (None, item["mode"]), "source and Git mode manifests disagree")
            files[name]["mode"] = item["mode"]
            seen.add(name)
    need(all(item["mode"] in ("100644", "100755") for item in files.values()),
         "every input needs a Git mode; provide a bound --mode-manifest if the original manifest lacks modes")
    return manifest, manifest_sha, mode_sha, files


def walk_archive(args, files, destination=None):
    archive = regular(args.archive, MAX_ARCHIVE)
    need(digest(archive, MAX_ARCHIVE) == args.archive_sha256, "archive SHA-256 differs")
    names, entries, seen, actual = {}, 0, set(), set()
    # No extract/extractall: regular files are copied through bounded streams.
    with tarfile.open(archive, "r:") as packed:
        need(packed.pax_headers.get("comment") == args.source_commit, "Git PAX commit differs or is absent")
        need(set(packed.pax_headers) == {"comment"}, "unsupported global PAX metadata")
        for member in packed:
            entries += 1
            need(entries <= MAX_ENTRIES, "archive entry budget exceeded")
            need(member.isdir() or member.isfile(), "links and nonregular TAR members are forbidden")
            need(not member.issparse() and not getattr(member, "sparse", None), "sparse TAR members are forbidden")
            need(set(member.pax_headers) <= {"comment", "path"}, "unsupported member PAX metadata")
            name = relative(member.name[:-1] if member.isdir() and member.name.endswith("/") else member.name)
            need(name.casefold() not in seen, f"duplicate archive member: {name}")
            seen.add(name.casefold())
            register(names, name, "directory" if member.isdir() else "file")
            if member.isdir():
                need(member.size == 0 and any(path.startswith(name + "/") for path in files), "unlisted/empty archive directory")
                continue
            need(name in files and name not in actual, f"unlisted archive file: {name}")
            expected = files[name]
            need(member.size == expected["bytes"] and member.size <= MAX_FILE, "archive file size differs")
            need(member.mode & ~0o777 == 0, "special file permission bits are forbidden")
            # Git tar.umask may affect group write bits, but cannot change the Git executable bit.
            mode = "100755" if member.mode & 0o111 else "100644"
            need(mode == expected["mode"], f"Git executable mode differs: {name}")
            output = None
            if destination is not None:
                target = plain(destination / name)
                need(target.is_relative_to(destination), "extraction path escapes destination")
                target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                plain(target.parent)
                output = target.open("xb")
            count, hashed = 0, hashlib.sha256()
            try:
                with packed.extractfile(member) as source:
                    for chunk in iter(lambda: source.read(1 << 20), b""):
                        count += len(chunk)
                        need(count <= expected["bytes"], "member exceeds declared byte budget")
                        hashed.update(chunk)
                        if output is not None:
                            output.write(chunk)
                need(count == expected["bytes"] and hashed.hexdigest() == expected["sha256"], f"file contents differ: {name}")
            finally:
                if output is not None:
                    output.close()
            if destination is not None:
                target.chmod(0o755 if mode == "100755" else 0o644)
                need(digest(target) == expected["sha256"], f"written file differs: {name}")
            actual.add(name)
        # Reject hidden concatenated members or other nonzero data after TAR EOF.
        padding = archive.stat().st_size - packed.offset
        need(padding >= 1024 and padding % 512 == 0, "Git TAR end padding is absent or malformed")
        packed.fileobj.seek(packed.offset)
        for chunk in iter(lambda: packed.fileobj.read(1 << 20), b""):
            need(not chunk.strip(b"\0"), "nonzero data follows the TAR end marker")
    need(actual == set(files), "archive source file inventory differs")
    need(digest(archive, MAX_ARCHIVE) == args.archive_sha256, "archive changed during validation/extraction")
    return {"archive_entries": entries, "files": len(actual), "bytes": sum(item["bytes"] for item in files.values())}


def native_destination(name):
    need(sys.platform == "linux", "snapshot extraction requires native Linux under the existing WSL guard")
    need(re.fullmatch(r"generic-nominals-[0-9]{2}", name), "snapshot name must be generic-nominals-NN")
    base = plain(BASE)
    need(base.is_dir(), "existing ext4 snapshot parent is required")
    destination = plain(base / name)
    need(not destination.exists(), "preserve existing snapshot; select a fresh name")
    mounts = []
    def unescape(value):
        return re.sub(r"\\([0-7]{3})", lambda match: chr(int(match[1], 8)), value)
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        left, separator, right = line.partition(" - ")
        need(separator, "invalid mount information")
        mountpoint, filesystem = Path(unescape(left.split()[4])), right.split()[0]
        if base.is_relative_to(mountpoint):
            mounts.append((len(mountpoint.parts), filesystem, str(mountpoint)))
    need(mounts, "snapshot parent mount cannot be identified")
    depth = max(item[0] for item in mounts)
    selected = {(filesystem, mountpoint) for length, filesystem, mountpoint in mounts if length == depth}
    need(len(selected) == 1, "ambiguous snapshot parent mount")
    filesystem, mountpoint = next(iter(selected))
    need(filesystem == "ext4", "snapshot parent must be on verified ext4")
    disk = shutil.disk_usage(base)
    if disk.free < MIN_DISK:
        raise ResourceHold("ext4 free disk below4GiB; capture refused")
    run = plain(os.environ.get("AXEON_RUN_DIR", ""))
    need(run.is_dir() and run.is_relative_to(EVIDENCE), "launch through the existing host/native guard with AXEON_RUN_DIR")
    parts = run.relative_to(EVIDENCE).parts
    need(len(parts) == 3 and re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", parts[0])
         and re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", parts[1]) and re.fullmatch(r"attempt-[0-9]{2,}", parts[2]),
         "dated Evidence attempt required")
    if shutil.disk_usage(run).free < MIN_DISK:
        raise ResourceHold("Windows Evidence free disk below4GiB; capture refused")
    return destination, run, {"filesystem": "ext4", "mountpoint": mountpoint, "free_disk_bytes": disk.free,
                             "minimum_free_disk_bytes": MIN_DISK}


def write_new(path, value):
    plain(path)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=True, allow_nan=False)
        stream.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("archive", "source-manifest"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("archive-sha256", "source-commit", "name"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--mode-manifest", type=Path)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    need(re.fullmatch(r"generic-nominals-[0-9]{2}", args.name), "snapshot name must be generic-nominals-NN")
    receipt, receipt_path = {"schema_version": 1, "status": "running", "validate_only": args.validate_only}, None
    code = 1
    try:
        destination = None
        if not args.validate_only:
            destination, run, admission = native_destination(args.name)
            planned_receipt = run / (args.name + "-capture.json")
            need(not planned_receipt.exists(), "preserve prior capture receipt; use a fresh Evidence attempt")
            receipt_path = planned_receipt
            receipt["destination"], receipt["disk_admission"] = str(destination), admission
        manifest, source_manifest_sha, mode_manifest_sha, files = expected_inputs(args)
        receipt.update(source_commit=args.source_commit, archive=str(regular(args.archive, MAX_ARCHIVE)),
                       archive_sha256=args.archive_sha256, source_manifest_sha256=source_manifest_sha,
                       mode_manifest_sha256=mode_manifest_sha, recipe_sha256=digest(__file__),
                       name=args.name, captured_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        receipt.update(walk_archive(args, files))
        need(digest(args.source_manifest) == source_manifest_sha
             and (args.mode_manifest is None or digest(args.mode_manifest) == mode_manifest_sha), "input manifests changed")
        if destination is not None:
            # Recheck admission after the complete read-only pass, before creating anything on ext4.
            destination, _, _ = native_destination(args.name)
            destination.mkdir(mode=0o700, exist_ok=False)
            walk_archive(args, files, destination)
            for name, item in files.items():
                path = plain(destination / name)
                need(digest(path) == item["sha256"] and stat.S_IMODE(path.stat().st_mode) == int(item["mode"][-3:], 8),
                     f"final extracted bytes/mode differ: {name}")
            need(digest(args.source_manifest) == source_manifest_sha
                 and (args.mode_manifest is None or digest(args.mode_manifest) == mode_manifest_sha), "input manifests changed")
            snapshot = {"schema_version": 1, "name": args.name, "captured_utc": receipt["captured_utc"],
                        "source": manifest.get("source"), "source_commit": args.source_commit,
                        "status": [], "source_state": "committed Git archive; no working-tree status inferred",
                        "archive_sha256": args.archive_sha256, "archive_format": "git-tar",
                        "files": [files[name] for name in sorted(files)],
                        "capture": {key: receipt[key] for key in ("source_manifest_sha256", "mode_manifest_sha256", "recipe_sha256")}}
            write_new(destination / ".snapshot-manifest.json", snapshot)
            receipt["snapshot_manifest_sha256"] = digest(destination / ".snapshot-manifest.json")
        receipt.update(status="passed", snapshot_created=destination is not None)
        code = 0
    except BaseException as error:
        receipt.update(status="resource_hold" if isinstance(error, ResourceHold) else "failed", error=f"{type(error).__name__}: {error}",
                       partial_destination_preserved=destination is not None and destination.exists())
        code = 75 if isinstance(error, ResourceHold) else 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        if receipt_path is not None:
            write_new(receipt_path, receipt)
    print(json.dumps(receipt, ensure_ascii=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
