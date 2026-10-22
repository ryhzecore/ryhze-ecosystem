"""Inert complete-evidence indexing and final-receipt failure contracts."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import evidence_manifest as manifest
import windows_ordinary as driver
from output_paths import evidence_directory
from source_manifest import file_sha256
from verify_desktop_surfaces import native_guard


def native(path):
    return Path("\\\\?\\" + str(path))


def put(path, data):
    host = native(path)
    host.parent.mkdir(parents=True, exist_ok=True)
    host.write_bytes(data)


def complete_cases(out, check):
    root = out / "complete"
    payloads = {"plain.txt": b"plain", "empty": b"", ".hidden": b"hidden",
                "nested/\u96ea/receipt.json": b"nested receipt", "nested/data.bin": bytes(range(256)),
                ".receipt-pending-old.json": b"retained failed publication"}
    for suffix in ("exe", "bat", "cmd", "com"):
        payloads["lower-" + suffix + "." + suffix] = b"inert executable-named bytes"
        payloads["upper-" + suffix + "." + suffix.upper()] = b"inert uppercase-named bytes"
    for units in (261, 320):
        leaf = "a" * 64 + ".rasset"
        base = root / "long" / "\u03bb-" / leaf
        padding = units - len(str(base).encode("utf-16-le")) // 2
        check("long fixture admits requested length " + str(units), 0 <= padding <= 253)
        key = "long/" + "\u03bb-" + "x" * padding + "/" + leaf
        check("long fixture retains exact units " + str(units),
              len(str(root / key).encode("utf-16-le")) // 2 == units)
        payloads[key] = b"RACEAST1" + bytes(436)
    base = root / "deep-directory" / "\u03bb-" / "child.bin"
    padding = 320 - len(str(base).encode("utf-16-le")) // 2
    check("deep directory fixture admits requested length", 0 <= padding <= 253)
    key = "deep-directory/\u03bb-" + "x" * padding + "/child.bin"
    check("scandir must enter a directory beyond 260 units",
          len(str(root / key).encode("utf-16-le")) // 2 == 320
          and len(str((root / key).parent).encode("utf-16-le")) // 2 > 260)
    payloads[key] = b"deep directory child"
    for key, raw in payloads.items():
        put(root / key, raw)
    put(root / "receipt.json", b"existing receipt is excluded, never rewritten")
    native(root / "empty-directory").mkdir()
    expected = {key: hashlib.sha256(raw).hexdigest() for key, raw in sorted(payloads.items())}
    check("complete coverage with only root receipt excluded", manifest.complete_files(root) == expected)
    check("ordinary keys use relative forward slashes", list(manifest.complete_files(root)) == list(expected))
    check("existing receipt unchanged", native(root / "receipt.json").read_bytes()
          == b"existing receipt is excluded, never rewritten")
    check("extended input yields same canonical keys", manifest.complete_files(native(root)) == expected)


def path_cases(out, check, rejects):
    check("ordinary path adds only extended prefix", manifest.host_path(out) == native(out))
    check("extended form is idempotent", manifest.host_path(native(out)) == native(out))
    unc = Path("\\\\inert-server\\share\\\u03bb")
    check("UNC is converted without access", str(manifest.host_path(unc)) == "\\\\?\\UNC\\inert-server\\share\\\u03bb")
    for path in (Path("relative"), Path("C:relative"), out / ".." / "escape"):
        rejects("noncanonical root refused " + str(path), lambda: manifest.complete_files(path))


def traversal_cases(out, check, rejects):
    root = out / "traversal"
    put(root / "one.txt", b"one")
    put(root / "nested/two.txt", b"two")
    scan = os.scandir

    def inaccessible(path):
        if Path(path) == native(root / "nested"):
            raise PermissionError("injected directory access denial")
        return scan(path)

    with patch.object(manifest.os, "scandir", new=inaccessible):
        rejects("nested traversal denial is not an empty directory", lambda: manifest.complete_files(root))

    @contextmanager
    def incomplete(path):
        with scan(path) as entries:
            if Path(path) == native(root):
                def interrupted():
                    yield next(entries)
                    raise OSError("injected mid-enumeration failure")
                yield interrupted()
            else:
                yield entries

    with patch.object(manifest.os, "scandir", new=incomplete):
        rejects("partial iteration cannot produce a complete manifest", lambda: manifest.complete_files(root))
    check("failed traversal preserves files", native(root / "nested/two.txt").read_bytes() == b"two")


def stat_cases(out, check, rejects):
    root = out / "classification"
    file = root / "file.bin"
    put(file, b"preserved")
    lstat = Path.lstat
    for fail_at, error in ((1, PermissionError), (2, FileNotFoundError),
                           (3, FileNotFoundError), (4, FileNotFoundError)):
        calls = 0

        def missing(path, *args, **kwargs):
            nonlocal calls
            if path == native(file):
                calls += 1
                if calls == fail_at:
                    raise error("injected classification or disappearance")
            return lstat(path, *args, **kwargs)

        with patch.object(Path, "lstat", new=missing):
            rejects("stat/disappearance propagates at observation " + str(fail_at),
                    lambda: manifest.complete_files(root))
        check("injected disappearance reached " + str(fail_at), calls == fail_at)
    check("disappearance injection never deletes source", native(file).read_bytes() == b"preserved")


def changed_stat(info, **changes):
    fields = ("st_dev", "st_ino", "st_mode", "st_size", "st_mtime_ns", "st_file_attributes")
    values = {name: getattr(info, name) for name in fields}
    values.update(changes)
    return SimpleNamespace(**values)


def signature_cases(check):
    directory = SimpleNamespace(st_dev=1, st_ino=2, st_mode=stat.S_IFDIR | 0o700,
                                st_size=0, st_mtime_ns=123456789, st_file_attributes=16)
    file = changed_stat(directory, st_mode=stat.S_IFREG | 0o600, st_size=8)
    for platform in ("nt", "posix"):
        with patch.object(manifest, "os", SimpleNamespace(name=platform)):
            check("directory size normalization scope " + platform,
                  (manifest.signature(directory) == manifest.signature(changed_stat(directory, st_size=4096)))
                  == (platform == "nt"))
            check("regular file size remains exact " + platform,
                  manifest.signature(file) != manifest.signature(changed_stat(file, st_size=9)))
            for field, value in (("st_dev", 9), ("st_ino", 9), ("st_mode", stat.S_IFREG),
                                 ("st_mtime_ns", directory.st_mtime_ns + 1)):
                check("directory signature retains " + platform + " " + field,
                      manifest.signature(directory) != manifest.signature(changed_stat(directory, **{field: value})))


def directory_size_cases(out, check):
    root = out / "directory-size"
    payloads = {"one.bin": b"one", "nested/two.bin": b"two"}
    for key, value in payloads.items():
        put(root / key, value)
    counts = {native(root): 0, native(root / "nested"): 0}
    lstat = Path.lstat

    def varying(path, *args, **kwargs):
        info = lstat(path, *args, **kwargs)
        if path in counts:
            counts[path] += 1
            return changed_stat(info, st_size=(counts[path] - 1) * 4096)
        return info

    with patch.object(Path, "lstat", new=varying):
        actual = manifest.complete_files(root)
    check("directory size variation preserves exact nested manifest",
          actual == {key: hashlib.sha256(value).hexdigest() for key, value in payloads.items()})
    check("directory size varies during enumeration and both snapshots", all(n >= 6 for n in counts.values()))


def directory_metadata_cases(out, check, rejects):
    root = out / "directory-metadata"
    put(root / "file.bin", b"preserved")
    lstat, scan, snapshot = Path.lstat, os.scandir, manifest.tree_snapshot
    baseline = lstat(native(root))
    changes = {"st_dev": baseline.st_dev + 1, "st_ino": baseline.st_ino + 1,
               "st_mode": stat.S_IFREG, "st_mtime_ns": baseline.st_mtime_ns + 1}
    for boundary in ("enumeration", "second-snapshot"):
        for field, value in changes.items():
            state = {"changed": False, "snapshots": 0, "injected": False, "error": ""}

            def altered(path, *args, **kwargs):
                info = lstat(path, *args, **kwargs)
                if path == native(root) and state["changed"]:
                    state["injected"] = True
                    return changed_stat(info, **{field: value})
                return info

            @contextmanager
            def enumerated(path):
                with scan(path) as entries:
                    yield entries
                if path == native(root) and boundary == "enumeration":
                    state["changed"] = True

            def snap(path):
                state["snapshots"] += 1
                if boundary == "second-snapshot" and state["snapshots"] == 2:
                    state["changed"] = True
                return snapshot(path)

            def collect():
                try:
                    return manifest.complete_files(root)
                except ValueError as error:
                    state["error"] = str(error)
                    raise

            with patch.object(Path, "lstat", new=altered), patch.object(manifest.os, "scandir", new=enumerated), \
                    patch.object(manifest, "tree_snapshot", new=snap):
                rejects("directory mutation rejected " + boundary + " " + field, collect)
            check("directory mutation reached " + boundary + " " + field, state["injected"])
            if boundary == "enumeration":
                check("directory failure retains bounded raw metadata " + field,
                      "raw (device,inode,type,size,mtime_ns) before=(" in state["error"]
                      and "after=(" in state["error"])


def file_size_cases(out, check, rejects):
    root = out / "file-size"
    file = root / "file.bin"
    put(file, b"preserved")
    lstat, fstat = Path.lstat, os.fstat
    for api, observation in (("path", 2), ("open", 1), ("close", 2), ("path", 3), ("path", 4)):
        calls = 0

        def alter(info):
            nonlocal calls
            calls += 1
            return changed_stat(info, st_size=info.st_size + 1) if calls == observation else info

        def path_stat(path, *args, **kwargs):
            info = lstat(path, *args, **kwargs)
            return alter(info) if api == "path" and path == native(file) else info

        def handle_stat(handle):
            info = fstat(handle)
            return alter(info) if api != "path" else info

        with patch.object(Path, "lstat", new=path_stat), patch.object(manifest.os, "fstat", new=handle_stat):
            rejects("file size mutation rejected " + api + str(observation), lambda: manifest.complete_files(root))
        check("file size mutation reached " + api + str(observation), calls == observation)


class BrokenRead:
    def __init__(self, stream):
        self.stream = stream
        self.reads = 0

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stream.close()

    def fileno(self):
        return self.stream.fileno()

    def read(self, size):
        self.reads += 1
        if self.reads == 2:
            raise OSError("injected interrupted read")
        return self.stream.read(size)


class ChangingRead(BrokenRead):
    def __init__(self, stream, mutate):
        super().__init__(stream)
        self.mutate = mutate

    def read(self, size):
        data = self.stream.read(size)
        if self.reads == 0:
            self.mutate()
        self.reads += 1
        return data


def content_mutation_cases(out, check, rejects):
    hash_file, open_file = manifest.hash_file, Path.open
    for boundary in ("before-hash", "during-read", "after-hash"):
        root = out / ("content-" + boundary)
        file = root / "file.bin"
        put(file, b"original")
        initial = native(file).stat()
        changed = False

        def mutate():
            nonlocal changed
            put(file, b"replaced")
            os.utime(native(file), ns=(initial.st_atime_ns, initial.st_mtime_ns + 1000000000))
            changed = True

        def hashed(path, expected):
            if boundary == "before-hash":
                mutate()
            result = hash_file(path, expected)
            if boundary == "after-hash":
                mutate()
            return result

        def opened(path, *args, **kwargs):
            stream = open_file(path, *args, **kwargs)
            if boundary == "during-read" and path == native(file) and args and args[0] == "rb":
                return ChangingRead(stream, mutate)
            return stream

        with patch.object(manifest, "hash_file", new=hashed), patch.object(Path, "open", new=opened):
            rejects("content mutation rejected " + boundary, lambda: manifest.complete_files(root))
        check("content mutation occurred and retained " + boundary,
              changed and native(file).read_bytes() == b"replaced")


def membership_cases(out, check, rejects):
    root = out / "membership"
    file = root / "file.bin"
    put(file, b"preserved")
    scan, snapshot = os.scandir, manifest.tree_snapshot
    scans = 0
    hidden = False

    def snap(path):
        nonlocal scans
        scans += 1
        return snapshot(path)

    @contextmanager
    def missing(path):
        nonlocal hidden
        with scan(path) as entries:
            if scans == 2 and path == native(root):
                hidden = True
                yield (entry for entry in entries if entry.name != file.name)
            else:
                yield entries

    with patch.object(manifest, "tree_snapshot", new=snap), patch.object(manifest.os, "scandir", new=missing):
        rejects("missing second-snapshot entry invalidates manifest", lambda: manifest.complete_files(root))
    check("missing membership injected without deleting evidence", hidden and native(file).read_bytes() == b"preserved")


def read_cases(out, check, rejects):
    root = out / "read-errors"
    file = root / "file.bin"
    put(file, b"preserved")
    open_file = Path.open
    for partial in (False, True):
        def unreadable(path, *args, **kwargs):
            if path == native(file):
                if partial:
                    return BrokenRead(open_file(path, *args, **kwargs))
                raise PermissionError("injected open denial")
            return open_file(path, *args, **kwargs)

        with patch.object(Path, "open", new=unreadable):
            rejects("open/read failure never returns partial hashes " + str(partial),
                    lambda: manifest.complete_files(root))
    hash_file = manifest.hash_file

    def new_entry(path, expected):
        result = hash_file(path, expected)
        put(root / "appeared.txt", b"late entry")
        return result

    with patch.object(manifest, "hash_file", new=new_entry):
        rejects("entry appearing after enumeration invalidates manifest", lambda: manifest.complete_files(root))
    check("read failures preserve original bytes", native(file).read_bytes() == b"preserved")


def redirection_cases(out, check, rejects):
    root = out / "redirection"
    file = root / "nested/file.bin"
    put(file, b"inside root")
    lstat = Path.lstat
    cases = [(root.parent, stat.S_IFDIR, 0x400), (root, stat.S_IFDIR, 0x400),
             (file.parent, stat.S_IFDIR, 0x400), (file, stat.S_IFREG, 0x400),
             (file, stat.S_IFLNK, 0), (file, stat.S_IFIFO, 0)]
    for number, (target, mode, attributes) in enumerate(cases):
        def redirected(path, *args, **kwargs):
            if path == native(target):
                return SimpleNamespace(st_mode=mode, st_file_attributes=attributes)
            return lstat(path, *args, **kwargs)

        with patch.object(Path, "lstat", new=redirected), patch.object(Path, "open") as opened:
            rejects("prohibited entry/ancestor rejected " + str(number), lambda: manifest.complete_files(root))
            check("prohibited entry rejected before file reads " + str(number), not opened.called)
    check("redirection cases are inert attribute injection", native(file).read_bytes() == b"inside root")


def stage_record(status, code):
    return {"passed": code == 0, "status": status, "exit_code": code,
            "error": "original stage diagnostic", "stages": [{"label": "test", "quiescent": True}]}


def finalization_cases(out, check):
    for status, code in (("passed", 0), ("held", 75), ("exited", 8), ("error", 125)):
        root = out / ("final-" + status)
        put(root / "stage/log.txt", b"retained stage output")
        record = stage_record(status, code)
        original = {k: record[k] for k in ("passed", "status", "exit_code", "error")}
        with patch.object(manifest, "complete_files", side_effect=OSError("injected manifest error")):
            driver.finalize_receipt(root, record)
        saved = json.loads(native(root / "receipt.json").read_text(encoding="utf-8"))
        check("manifest failure overrides success/hold while retaining stage outcome " + status,
              saved == record and saved["stage_outcome"] == original and not saved["passed"]
              and saved["exit_code"] == 125 and saved["status"] == "evidence-incomplete"
              and saved["manifest"]["complete"] is False and saved["files"] == {})
        check("stage diagnostics remain intact " + status,
              saved["error"] == original["error"] and saved["stages"] == record["stages"])
    root = out / "final-complete"
    put(root / "log.txt", b"complete")
    record = stage_record("passed", 0)
    driver.finalize_receipt(root, record)
    check("complete index preserves stage success", record["passed"] and record["manifest"]["complete"]
          and record["files"] == {"log.txt": hashlib.sha256(b"complete").hexdigest()})
    check("successful publication leaves no unindexed temporary file",
          sorted(p.name for p in native(root).iterdir()) == ["log.txt", "receipt.json"])


class BrokenClose:
    def __init__(self, stream):
        self.stream = stream

    def __getattr__(self, name):
        return getattr(self.stream, name)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stream.close()
        raise OSError("injected receipt close failure")


def publication_cases(out, check, rejects):
    open_file = Path.open

    def partial_write(value, stream, **_):
        stream.write('{"passed": true, "incomplete":')
        raise OSError("injected failure after partial receipt bytes")

    for failure in ("open", "serialize", "fsync", "close", "rename"):
        root = out / ("publish-" + failure)
        native(root).mkdir()
        record = stage_record("passed", 0)
        if failure in ("open", "close"):
            def failed_open(path, *args, **kwargs):
                if args and args[0] == "x":
                    if failure == "open":
                        raise PermissionError("injected receipt open failure")
                    return BrokenClose(open_file(path, *args, **kwargs))
                return open_file(path, *args, **kwargs)
            injection = patch.object(Path, "open", new=failed_open)
        elif failure == "serialize":
            injection = patch.object(manifest.json, "dump", new=partial_write)
        else:
            injection = patch.object(manifest.os, failure, side_effect=OSError("injected " + failure))
        with injection:
            rejects("receipt publication failure propagates " + failure,
                    lambda: driver.finalize_receipt(root, record))
        check("no success receipt after publication failure " + failure,
              not native(root / "receipt.json").exists() and not record["passed"]
              and record["exit_code"] == 125 and record["status"] == "receipt-write-failed"
              and record["stage_outcome"]["passed"])
    root = out / "no-overwrite"
    put(root / "receipt.json", b"prior receipt")
    rejects("existing final receipt cannot be replaced",
            lambda: manifest.write_receipt(root, {"passed": True}))
    check("existing receipt bytes preserved", native(root / "receipt.json").read_bytes() == b"prior receipt")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    out = evidence_directory("quality-step0-evidence-manifest-tests", args.output)
    sources = (manifest.SELF, driver.SELF, Path(__file__))
    receipt = {"schema": 1, "status": "checking", "checks": [], "native_started": False,
               "sources": {str(p): file_sha256(p) for p in sources}}

    def check(name, value):
        receipt["checks"].append({"check": name, "passed": bool(value)})
        if not value:
            raise AssertionError(name)

    def rejects(name, action):
        try:
            action()
        except (OSError, ValueError):
            check(name, True)
        else:
            check(name, False)

    try:
        receipt["guard"] = native_guard(out)
        if not receipt["guard"]["passed"]:
            receipt["status"] = "held"
        else:
            check("Windows inert fixture", os.name == "nt")
            path_cases(out, check, rejects)
            complete_cases(out, check)
            traversal_cases(out, check, rejects)
            stat_cases(out, check, rejects)
            signature_cases(check)
            directory_size_cases(out, check)
            directory_metadata_cases(out, check, rejects)
            file_size_cases(out, check, rejects)
            content_mutation_cases(out, check, rejects)
            membership_cases(out, check, rejects)
            read_cases(out, check, rejects)
            redirection_cases(out, check, rejects)
            finalization_cases(out, check)
            publication_cases(out, check, rejects)
            receipt["status"] = "passed"
    except BaseException as error:
        receipt.update(status="failed", error=repr(error))
    finally:
        receipt["source_unchanged"] = all(file_sha256(p) == digest for p, digest in
                                         ((Path(p), digest) for p, digest in receipt["sources"].items()))
        if not receipt["source_unchanged"]:
            receipt.update(status="failed", error="Source changed during inert checks")
        with (out / "tests.json").open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2, sort_keys=True)
    print(json.dumps({"status": receipt["status"], "checks": len(receipt["checks"]),
                      "error": receipt.get("error"), "receipt": str(out / "tests.json")}))
    return 0 if receipt["status"] == "passed" else 75 if receipt["status"] == "held" else 1


if __name__ == "__main__":
    raise SystemExit(main())
