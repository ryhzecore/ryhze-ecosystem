"""Independent native binary typed-index publication and rejection oracle."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
import subprocess

from output_paths import evidence_path


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical(value) -> bytes:
    return (json.dumps(value, separators=(",", ":")) + "\n").encode("ascii")


def parse_binary_index(data: bytes) -> dict:
    """Independent binary v1 validation; no native encoder/decoder is imported."""
    if not 64 <= len(data) <= 8192 or data[:8] != b"RACEPRJ1":
        raise ValueError("binary project magic or byte bound")
    version, header, total, count, entry_size, reserved = struct.unpack_from("<6I", data, 8)
    if (version, header, entry_size, reserved) != (1, 32, 152, 0) or count > 32 or total != len(data) or total != 64 + 152 * count:
        raise ValueError("binary project version, extent, count or reserved field")
    if data[-32:] != hashlib.sha256(data[:-32]).digest():
        raise ValueError("binary project checksum")
    rows, leaves, previous = [], set(), -1
    kinds = {1: "mesh", 2: "texture", 3: "material"}
    for n in range(count):
        high, low, revision, kind, size, padded = struct.unpack_from("<QQ32sII96s", data, 32 + 152 * n)
        identity = high << 64 | low
        if identity == 0 or identity <= previous or kind not in kinds or revision == bytes(32):
            raise ValueError("binary project identity/order/kind/revision")
        if not 1 <= size <= 96 or any(padded[size:]):
            raise ValueError("binary project leaf size or nonzero padding")
        try:
            leaf = padded[:size].decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError("binary project non-ASCII leaf") from error
        if not re.fullmatch(r"[a-z0-9-][a-z0-9.-]*\.json", leaf) or ".." in leaf or leaf in leaves:
            raise ValueError("binary project canonical unique recipe leaf")
        rows.append({"asset": f"{identity:032x}", "kind": kinds[kind], "recipe": leaf, "revision": revision.hex()})
        leaves.add(leaf)
        previous = identity
    return {"schema": 2, "recipes": rows}


def binary_index(references) -> bytes:
    """Independent canonical LE wire oracle for exact authoring comparisons."""
    rows = sorted(references, key=lambda value: int(value["asset"], 16))
    if len(rows) > 32:
        raise ValueError("binary project reference count")
    kinds = {"mesh": 1, "texture": 2, "material": 3}
    body = bytearray(struct.pack("<8s6I", b"RACEPRJ1", 1, 32, 64 + 152 * len(rows), len(rows), 152, 0))
    for row in rows:
        leaf = row["recipe"].encode("ascii")
        if len(leaf) > 96:
            raise ValueError("binary project recipe leaf length")
        revision = bytes.fromhex(row["revision"])
        if len(revision) != 32:
            raise ValueError("binary project revision length")
        body.extend(struct.pack("<QQ32sII96s", int(row["asset"][:16], 16), int(row["asset"][16:], 16),
                                revision, kinds[row["kind"]], len(leaf), leaf))
    result = bytes(body) + hashlib.sha256(body).digest()
    parse_binary_index(result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--surface-tool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = evidence_path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    root = output / "recipes"
    root.mkdir()
    checks, calls = [], []
    tools = [args.tool.resolve(), args.surface_tool.resolve()]
    hashes = {str(p): digest(p.read_bytes()) for p in tools}
    receipt = {"passed": False, "checks": checks, "calls": calls, "executables": hashes}

    def check(name, condition):
        checks.append({"name": name, "passed": bool(condition)})
        if not condition:
            raise AssertionError(name)

    def invoke(tool, *arguments, success=True, identity=False, expected_exit=None):
        command = [str(tool), *map(str, arguments)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        calls.append({"command": command, "exit_code": result.returncode,
                      "stdout": result.stdout, "stderr": result.stderr,
                      "expected_success": success, "expected_exit": expected_exit if expected_exit is not None else (0 if success else 1)})
        check(f"command {len(calls)} exit", result.returncode == (expected_exit if expected_exit is not None else (0 if success else 1)))
        if not success:
            return None
        if identity:
            return result.stdout.strip().split()
        value = json.loads(result.stdout)
        check(f"command {len(calls)} source identity",
              [value["source_commit"], value["authored_sha256"]] == build_identity)
        return value

    def snapshot():
        return {p.name: digest(p.read_bytes()) for p in root.iterdir() if p.is_file()}

    def reject_unchanged(*arguments, expected_exit=1):
        before = snapshot()
        invoke(args.tool, *arguments, success=False, expected_exit=expected_exit)
        check(f"command {len(calls)} immutable files", snapshot() == before)

    try:
        build_identity = invoke(args.tool, "--identity", identity=True)
        check("well-formed source identity", len(build_identity) == 2 and
              len(build_identity[0]) == 40 and len(build_identity[1]) == 64)
        check("same surface/content source identity",
              invoke(args.surface_tool, "--identity", identity=True) == build_identity)
        receipt["source_commit"], receipt["authored_sha256"] = build_identity
        reject_unchanged("create", root, "json-index-disallowed.json")
        reject_unchanged("create-json", root, "empty.json", expected_exit=2)
        reject_unchanged("convert", root, "legacy.json", "converted.raceproject", expected_exit=2)
        binary_empty = invoke(args.tool, "create", root, "empty.raceproject")
        empty_wire = binary_index([])
        check("empty binary fixed golden", digest(empty_wire) == "8396bcc3ac7cfefe3555853aaf6dbcdd1f6bae9b6887951820c491991b75a184")
        check("empty binary exact publication", (root / "empty.raceproject").read_bytes() == empty_wire)
        check("empty binary receipt", binary_empty["encoding"] == "binary" and binary_empty["wire_version"] == 1
              and binary_empty["schema"] == 2 and binary_empty["entries"] == 0 and binary_empty["revision"] == digest(empty_wire))
        check("empty independent parse", parse_binary_index(empty_wire) == {"schema": 2, "recipes": []})
        check("empty create remains idempotent", invoke(args.tool, "create", root, "empty.raceproject") == binary_empty)

        # Independent legacy mesh recipe construction, not a call to its encoder.
        mesh_id, texture_id, material_id = (f"{n:032x}" for n in (1, 2, 3))
        legacy_recipe = {"schema": 1, "asset": mesh_id, "source": "mesh.obj",
                         "mesh_container": 1, "mesh_schema": 1, "target": 1,
                         "settings": digest(b"race-obj-triangles-settings-1:metres;right-handed;y-up;ccw;flat-normals;binary64"),
                         "importer": digest(b"race-first-party-obj-triangles-importer-1"),
                         "cooker": digest(b"race-native-mesh-portable-cpu-cooker-1"),
                         "dependencies": digest(b"")}
        mesh_bytes = canonical(legacy_recipe)
        (root / "mesh.json").write_bytes(mesh_bytes)
        tex = invoke(args.surface_tool, "create-recipe", root, "texture.json", texture_id,
                     "color.bmp", "bmp-srgb-mips")
        mat = invoke(args.surface_tool, "create-recipe", root, "material.json", material_id,
                     "paint.json", "material")
        references = [
            {"asset": mesh_id, "kind": "mesh", "recipe": "mesh.json", "revision": digest(mesh_bytes)},
            {"asset": texture_id, "kind": "texture", "recipe": "texture.json", "revision": tex["recipe_revision"]},
            {"asset": material_id, "kind": "material", "recipe": "material.json", "revision": mat["recipe_revision"]}]

        def arguments(refs):
            return [part for r in refs for part in (r["kind"], r["asset"], r["recipe"], r["revision"])]

        mixed_bytes = canonical({"schema": 2, "recipes": references})
        project = invoke(args.tool, "create", root, "mixed.raceproject", *arguments(reversed(references)))
        mixed_wire = binary_index(references)
        check("mixed sorted binary exact bytes", (root / "mixed.raceproject").read_bytes() == mixed_wire)
        check("mixed independent parsed pins", parse_binary_index(mixed_wire) == {"schema": 2, "recipes": references})
        check("mixed raw binary revision", project["revision"] == digest(mixed_wire))
        one_wire = binary_index(references[:1])
        check("one mesh binary fixed golden", digest(one_wire) == "215fb29fe2e954cebcecadea701b623495381d30ef428e5c96150c911ae34ec8")
        expected_rows = [{"kind": r["kind"], "id": r["asset"], "source": source,
                          "recipe": r["recipe"], "recipe_revision": r["revision"], "profile": profile}
                         for r, source, profile in zip(references, ("mesh.obj", "color.bmp", "paint.json"),
                                                        ("geometry", "bmp-srgb-mips", "material"))]
        check("complete typed rows", project["recipes"] == expected_rows)
        check("typed report metadata", project["format"] == "RACECONTENT_PROJECT" and
              project["schema"] == 2 and project["leaf"] == "mixed.raceproject" and project["entries"] == 3
              and project["encoding"] == "binary" and project["wire_version"] == 1)
        check("bounded retained closure", 0 < project["retained_bytes"] <= 256 * 1024)
        inspected = invoke(args.tool, "inspect", root, "mixed.raceproject")
        check("inspect same closure", inspected == project)
        invoke(args.tool, "create", root, "mixed.raceproject", *arguments(references))
        reject_unchanged("create", root, "mixed.raceproject")
        # Neither JSON schema is an index input, even under the native extension.
        # Both rejections must preserve the recipes and already-authored binary files.
        legacy_ref = {k: references[0][k] for k in ("asset", "recipe", "revision")}
        for schema, payload in ((1, canonical({"schema": 1, "recipes": [legacy_ref]})),
                                (2, mixed_bytes)):
            for extension in ("json", "raceproject"):
                leaf = f"rejected-json{schema}.{extension}"
                (root / leaf).write_bytes(payload)
                reject_unchanged("inspect", root, leaf)
        (root / "mixed.json").write_bytes(mixed_wire)
        reject_unchanged("inspect", root, "mixed.json")
        # A second canonical index can select an alias of the exact same recipe:
        # closure pins stay equal while the project revision changes meaningfully.
        (root / "mesh-alias.json").write_bytes(mesh_bytes)
        alias_refs = [{**references[0], "recipe": "mesh-alias.json"}, *references[1:]]
        alias = invoke(args.tool, "create", root, "alias.raceproject", *arguments(alias_refs))
        check("alias closure preserves exact recipe revisions", [r["recipe_revision"] for r in alias["recipes"]]
              == [r["recipe_revision"] for r in project["recipes"]])
        check("canonical index revision includes recipe leaf", alias["revision"] == digest(binary_index(alias_refs))
              and alias["revision"] != project["revision"])

        wrong_kind = [dict(r) for r in references]
        wrong_kind[1]["kind"] = "material"
        reject_unchanged("create", root, "wrong-kind.raceproject", *arguments(wrong_kind))
        duplicate = [references[0], {**references[1], "asset": mesh_id}]
        reject_unchanged("create", root, "duplicate.raceproject", *arguments(duplicate))
        missing = [{**references[2], "recipe": "missing.json"}]
        reject_unchanged("create", root, "missing-index.raceproject", *arguments(missing))
        stale = [{**references[2], "revision": "1" * 64}]
        reject_unchanged("create", root, "stale-index.raceproject", *arguments(stale))
        reject_unchanged("create", root, "wrong-kind.raceproject", "scene", mesh_id, "mesh.json", digest(mesh_bytes))
        reject_unchanged("create", root, "../escape.raceproject")
        check("traversal did not write outside root", not (output / "escape.raceproject").exists())
        reject_unchanged("create", root, "too-many.raceproject", *arguments([references[0]] * 33), expected_exit=2)

        malformed = [b"", b"\xef\xbb\xbf" + mixed_bytes, mixed_bytes + b"{}",
                     mixed_bytes.replace(b'"schema":2', b'"schema":02'),
                     mixed_bytes.replace(b'"schema":2', b'"schema":3'),
                     mixed_bytes.replace(b'"schema":2', b'"schema":2,"schema":2'),
                     mixed_bytes.replace(b'"recipes":', b'"unknown":'),
                     mixed_bytes.replace(b'"kind":"texture"', b'"kind":"mesh"'),
                     b" " * 8193]
        for n, data in enumerate(malformed):
            leaf = f"invalid-{n}.raceproject"
            (root / leaf).write_bytes(data)
            reject_unchanged("inspect", root, leaf)
        # Exercise the exact maximum and full 96-byte leaf slot, independently
        # constructing every mesh recipe and reference; no native encoder oracle.
        maximum_refs = []
        for n in range(32):
            identity = f"{100 + n:032x}"
            leaf = "r" * 91 + ".json" if n == 0 else f"maximum-{n:02d}.json"
            raw = canonical({**legacy_recipe, "asset": identity})
            (root / leaf).write_bytes(raw)
            maximum_refs.append({"asset": identity, "kind": "mesh", "recipe": leaf, "revision": digest(raw)})
        maximum = invoke(args.tool, "create", root, "maximum.raceproject", *arguments(reversed(maximum_refs)))
        maximum_wire = binary_index(maximum_refs)
        check("32-entry maximum exact binary", (root / "maximum.raceproject").read_bytes() == maximum_wire
              and len(maximum_wire) == 4928 and maximum["entries"] == 32)
        check("96-byte leaf retained", parse_binary_index(maximum_wire)["recipes"][0]["recipe"] == maximum_refs[0]["recipe"])

        def resealed(data):
            return bytes(data[:-32]) + hashlib.sha256(data[:-32]).digest()

        binary_bad = []
        for end in (0, 1, 7, 8, 12, 31, 32, 63, len(mixed_wire) - 33, len(mixed_wire) - 1):
            binary_bad.append((f"truncated-{end}", mixed_wire[:end]))
        binary_bad += [("trailing", mixed_wire + b"x"), ("bound", mixed_wire + bytes(8193 - len(mixed_wire))),
                       ("checksum", mixed_wire[:-1] + bytes([mixed_wire[-1] ^ 1]))]
        for label, offset, value in (
                ("version-zero", 8, 0), ("version-future", 8, 2), ("header", 12, 31),
                ("total-short", 16, len(mixed_wire) - 1), ("total-long", 16, len(mixed_wire) + 1),
                ("count-zero", 20, 0), ("count-short", 20, 2), ("count-long", 20, 4),
                ("count-bound", 20, 33), ("count-overflow", 20, 0xffffffff),
                ("entry-size", 24, 151), ("reserved", 28, 1),
                ("kind-zero", 32 + 48, 0), ("kind-unknown", 32 + 48, 4), ("kind-wide", 32 + 48, 257),
                ("leaf-empty", 32 + 52, 0), ("leaf-bound", 32 + 52, 97)):
            changed = bytearray(mixed_wire)
            struct.pack_into("<I", changed, offset, value)
            binary_bad.append((label, resealed(changed)))
        for label, offset, data in (
                ("magic", 0, b"RACEXRJ1"), ("zero-id", 32, bytes(16)), ("zero-revision", 48, bytes(32)),
                ("leaf-uppercase", 88, b"M"), ("leaf-nul", 88, b"\0"), ("leaf-nonascii", 88, b"\xff"),
                ("leaf-path", 88, b"../x.json"), ("leaf-extension", 88, b"mesh.jsom"),
                ("leaf-padding", 88 + len("mesh.json"), b"x"),
                ("duplicate-id", 32 + 152, mixed_wire[32:48]),
                ("duplicate-leaf", 32 + 152 + 52, mixed_wire[32 + 52:32 + 152])):
            changed = bytearray(mixed_wire)
            changed[offset:offset + len(data)] = data
            binary_bad.append((label, resealed(changed)))
        reversed_entries = mixed_wire[:32] + mixed_wire[336:488] + mixed_wire[184:336] + mixed_wire[32:184] + mixed_wire[-32:]
        binary_bad.append(("noncanonical-order", resealed(reversed_entries)))
        for label, data in binary_bad:
            leaf = "binary-invalid-" + label + ".raceproject"
            (root / leaf).write_bytes(data)
            try:
                parse_binary_index(data)
            except ValueError:
                check(label + " independent rejection", True)
            else:
                check(label + " independent rejection", False)
            reject_unchanged("inspect", root, leaf)
        (root / "binary-disguised.json").write_bytes(mixed_wire)
        (root / "json-disguised.raceproject").write_bytes(mixed_bytes)
        reject_unchanged("inspect", root, "binary-disguised.json")
        reject_unchanged("inspect", root, "json-disguised.raceproject")

        # Recipe replacement cannot retarget an existing exact project pin.
        (root / "mesh.json").write_bytes(mesh_bytes + b" ")
        reject_unchanged("inspect", root, "mixed.raceproject")
        check("original index retained", (root / "mixed.raceproject").read_bytes() == mixed_wire)
        for p in tools:
            check(p.name + " unchanged", digest(p.read_bytes()) == hashes[str(p)])
        receipt["passed"] = True
    except Exception as exc:
        receipt["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": receipt["passed"], "checks": len(checks), "calls": len(calls),
                      "receipt": str(output / "receipt.json"), "error": receipt.get("error")}))
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
