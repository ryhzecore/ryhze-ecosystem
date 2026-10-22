"""Complete quiescent attempt indexing; no native execution or source selection."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import uuid

SELF = Path(__file__).resolve()


def host_path(path: Path) -> Path:
    path = Path(path)
    if not path.is_absolute() or ".." in path.parts:
        raise ValueError(f"Absolute canonical evidence path required: {path}")
    if os.name != "nt":
        return path
    value = str(path)
    if value.startswith("\\\\?\\"):
        return path
    if value.startswith("\\\\"):
        return Path("\\\\?\\UNC\\" + value[2:])
    return Path("\\\\?\\" + value)


def checked_stat(path: Path):
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        raise ValueError(f"Redirected evidence refused: {path}")
    if not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
        raise ValueError(f"Unsupported evidence entry: {path}")
    return info


def directory_chain(path: Path):
    for part in (*reversed(path.parents), path):
        if not stat.S_ISDIR(checked_stat(part).st_mode):
            raise ValueError(f"Evidence ancestor is not a directory: {part}")


def raw_signature(info):
    # Windows path stat adds filename-derived execute bits that handle fstat
    # does not. Compare the shared file type, not those synthetic permissions.
    return info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode), info.st_size, info.st_mtime_ns


def signature(info):
    # Reads can change access time and Windows directory size without changing
    # entries. Keep file sizes exact; directory identity, type, mtime and complete
    # membership still have to match across the two snapshots.
    values = raw_signature(info)
    if os.name == "nt" and stat.S_ISDIR(info.st_mode):
        return values[:3] + (0, values[4])
    return values


def tree_snapshot(root: Path) -> dict:
    directory_chain(root)
    result = {".": signature(checked_stat(root))}
    pending = [root]
    while pending:
        directory = pending.pop()
        before = checked_stat(directory)
        if not stat.S_ISDIR(before.st_mode):
            raise ValueError(f"Evidence directory changed type: {directory}")
        with os.scandir(directory) as entries:
            names = sorted(entry.name for entry in entries)
        for name in names:
            path = directory / name
            info = checked_stat(path)
            key = path.relative_to(root).as_posix()
            if key in result:
                raise ValueError(f"Duplicate evidence entry: {key}")
            result[key] = signature(info)
            if stat.S_ISDIR(info.st_mode):
                pending.append(path)
        after = checked_stat(directory)
        if signature(after) != signature(before):
            raise ValueError(f"Evidence directory changed during enumeration: {directory}; "
                             f"raw (device,inode,type,size,mtime_ns) before={raw_signature(before)!r} "
                             f"after={raw_signature(after)!r}")
    return result


def hash_file(path: Path, expected: tuple) -> str:
    if signature(checked_stat(path)) != expected:
        raise ValueError(f"Evidence file changed before hashing: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        if signature(os.fstat(stream.fileno())) != expected:
            raise ValueError(f"Evidence opened identity differs: {path}")
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
        if signature(os.fstat(stream.fileno())) != expected:
            raise ValueError(f"Evidence file changed while hashing: {path}")
    if signature(checked_stat(path)) != expected:
        raise ValueError(f"Evidence file changed after hashing: {path}")
    return digest.hexdigest()


def complete_files(output: Path) -> dict:
    """Hash every regular file except the root receipt; propagate every I/O error.

    The caller has stopped writers. Two complete metadata traversals detect
    disappearance or mutation during collection; this is not a hostile-writer
    atomic filesystem snapshot.
    """
    root = host_path(output)
    before = tree_snapshot(root)
    files = {}
    for key, info in sorted(before.items()):
        if key != "receipt.json" and stat.S_ISREG(info[2]):
            files[key] = hash_file(root / key, info)
    if tree_snapshot(root) != before:
        raise ValueError("Evidence tree changed during manifest collection")
    return files


def write_receipt(output: Path, record: dict):
    """Publish one complete Windows receipt without replacing an existing file.

    A write/flush/close/rename failure leaves a diagnostic pending file, never a
    newly published success receipt. The pending file is created after indexing
    and renamed away on success, so only receipt.json is excluded from the index.
    """
    if os.name != "nt":
        raise ValueError("Non-replacing receipt publication requires Windows")
    root = host_path(output)
    directory_chain(root)
    pending = root / (".receipt-pending-" + uuid.uuid4().hex + ".json")
    try:
        with pending.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(record, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        directory_chain(root)
        checked_stat(pending)
        # Windows rename refuses an existing destination; never use replace().
        os.rename(pending, root / "receipt.json")
    except (Exception, KeyboardInterrupt) as error:
        raise OSError(f"Final receipt not published; pending path {pending}: {error}") from error
