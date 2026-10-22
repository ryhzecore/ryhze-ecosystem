"""Independent surface-recipe CLI and native-wire oracles; retain every failure."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct

from output_paths import evidence_directory
from test_surface_import import (GOLDEN_RGB, IDENTITY, MIP_COOKER, MIP_SETTINGS,
                                 SCALAR_MIP_COOKER, SCALAR_MIP_SETTINGS,
                                 SCALAR_MATERIAL_SETTINGS, SCALAR_MATERIAL_IMPORTER, SCALAR_MATERIAL_COOKER,
                                 Harness, bmp, digest, native_wire, ppm, rgb_fixture, snapshot)

MATERIAL_ID = "00000000000000050000000000000009"
PROFILES = ("ppm-srgb", "ppm-linear", "ppm-srgb-mips", "bmp-srgb", "bmp-linear",
            "bmp-srgb-mips", "material", "material-mips", "ppm-scalar-mips", "bmp-scalar-mips", "material-scalar-mips")


def key_bytes(pin: tuple[str, str] | None) -> bytes:
    return (struct.pack("<QQ", int(pin[0][:16], 16), int(pin[0][16:], 16))
            + bytes.fromhex(pin[1])) if pin else b""


def tuple_strings(profile: str) -> tuple[str, str, str, str]:
    """Authored contract constants, independent of production profile helpers."""
    material, mips = profile.startswith("material"), profile.endswith("mips")
    if profile == "material-scalar-mips":
        return (SCALAR_MATERIAL_SETTINGS, SCALAR_MATERIAL_SETTINGS, SCALAR_MATERIAL_IMPORTER, SCALAR_MATERIAL_COOKER)
    if material:
        settings = ("race-opaque-diffuse-2:linear-rgb;linear-clamp-mips;complete-cooked-srgb-texture"
                    if mips else "race-opaque-diffuse-1:linear-rgb;linear-clamp-mip0")
        return (settings, settings, "race-material-json-importer-" + ("2" if mips else "1"),
                "race-opaque-diffuse-portable-cpu-cooker-2:linear-clamp-mips" if mips
                else "race-opaque-diffuse-portable-cpu-cooker-1")
    source_format = profile.split("-")[0]
    scalar = profile.endswith("-scalar-mips")
    imported = f"race-{source_format}-1:rgba8;top-left;{'linear-data' if scalar or profile.endswith('linear') else 'srgb'};opaque"
    return (imported, SCALAR_MIP_SETTINGS if scalar else MIP_SETTINGS if mips else imported,
            "race-bmp-bgr24-info40-importer-1" if source_format == "bmp" else "race-ppm-p6-importer-1",
            SCALAR_MIP_COOKER if scalar else MIP_COOKER if mips else "race-rgba8-portable-cpu-cooker-1")


def definition(profile: str, source: str, identity: str = IDENTITY,
               pin: tuple[str, str] | None = None) -> dict:
    imported, settings, importer, cooker = tuple_strings(profile)
    return {"schema": 1, "asset": identity, "source": source, "profile": profile,
            "asset_container": 1, "asset_schema": 3 if profile.endswith("-scalar-mips") else 2 if profile.endswith("mips") else 1,
            "target": 1, "import_settings": digest(imported.encode()).hex(),
            "settings": digest(settings.encode()).hex(), "importer": digest(importer.encode()).hex(),
            "cooker": digest(cooker.encode()).hex(), "dependencies": digest(key_bytes(pin)).hex(),
            "texture_asset": pin[0] if pin else "0" * 32,
            "texture_revision": pin[1] if pin else "0" * 64}


def canonical(value: dict) -> bytes:
    return (json.dumps(value, separators=(",", ":")) + "\n").encode("ascii")


def material_source(profile: str, pin: tuple[str, str] | None,
                    color=(1, 0.25, 0.1)) -> bytes:
    body = {"schema": 3 if profile == "material-scalar-mips" else 2 if profile.endswith("mips") else 1, "model": "opaque-diffuse",
            "base_color": list(color), "base_color_texture":
            {"asset": pin[0], "revision": pin[1]} if pin else None}
    if profile.endswith("mips"):
        body["sampling"] = "linear-clamp-mips"
    if profile == "material-scalar-mips":
        body["texture_role"] = "diffuse-reflectance-r"
    return canonical(body)


def material_wire(source: bytes, profile: str, pin: tuple[str, str] | None,
                  *, cooked: bool, color=(1, 0.25, 0.1)) -> bytes:
    """Independent documented material envelope, payload, and SHA-256 checksum."""
    imported, settings, importer, cooker = tuple_strings(profile)
    schema = 3 if profile == "material-scalar-mips" else 2 if profile.endswith("mips") else 1
    payload = struct.pack("<3d", *color) + (key_bytes(pin) if pin else bytes(48))
    header = bytearray(256)
    header[:8] = b"RACEAST1"
    struct.pack_into("<6I", header, 8, 1, 256, 3, schema, 2 if cooked else 1, 1 if cooked else 0)
    struct.pack_into("<6Q", header, 32, 360, 256, 72, 0, 5, 9)
    header[80:112] = digest(source)
    header[112:144] = digest((settings if cooked else imported).encode())
    header[144:176] = digest(importer.encode())
    if cooked:
        header[176:208] = digest(cooker.encode())
    header[208:240] = digest(key_bytes(pin))
    struct.pack_into("<4I", header, 240, 1, bool(pin), 1 if schema == 1 else 2, 2 if schema == 3 else 0)
    body = bytes(header) + payload
    return body + digest(body)


class RecipeHarness(Harness):
    def __init__(self, tool: Path, root: Path, expected_identity: list[str] | None):
        super().__init__(tool, root)
        self.expected_identity = expected_identity
        for name in ("recipes", "direct", "missing-dependency", "wrong-dependency"):
            (root / name).mkdir()
        self.products: dict[str, dict] = {}
        self.report["scope"] = "Eleven bounded offline surface recipes; exact independent wire/profile and retained reimport oracles. No editor, runtime GPU or performance claim."

    def create(self, label: str, spec: dict) -> bytes:
        leaf = label + ".json"
        args = ["create-recipe", self.root / "recipes", leaf, spec["asset"], spec["source"], spec["profile"]]
        if spec["texture_asset"] != "0" * 32:
            args += [spec["texture_asset"], spec["texture_revision"]]
        result = self.invoke(*args)
        expected = canonical(spec)
        self.check(label + "-canonical-recipe", (self.root / "recipes" / leaf).read_bytes() == expected)
        self.check(label + "-creation-receipt", result["format"] == "RACESURFACE_RECIPE1"
                   and result["schema"] == 1 and result["id"] == spec["asset"]
                   and result["source"] == spec["source"] and result["recipe_profile"] == spec["profile"]
                   and result["recipe_leaf"] == leaf and result["recipe_revision"] == digest(expected).hex())
        self.check(label + "-idempotent-create", self.invoke(*args) == result)
        return expected

    def execute(self, label: str, raw: bytes, spec: dict, source: bytes,
                imported: bytes, cooked: bytes, *, output: Path | None = None) -> dict:
        output = output or self.root / "products"
        result = self.invoke("recipe", self.root / "recipes", label + ".json", self.root / "sources", output)
        expected_revision = digest(cooked).hex()
        self.check(label + "-execution-identity", result["recipe_revision"] == digest(raw).hex()
                   and result["recipe_profile"] == spec["profile"] and result["source"] == digest(source).hex()
                   and result["id"] == spec["asset"])
        self.check(label + "-imported-wire", result["imported_revision"] == digest(imported).hex()
                   and result["imported_leaf"] == digest(imported).hex() + ".rasset"
                   and (output / result["imported_leaf"]).read_bytes() == imported)
        self.check(label + "-cooked-wire", result["revision"] == expected_revision
                   and result["leaf"] == expected_revision + ".rasset"
                   and (output / result["leaf"]).read_bytes() == cooked)
        self.check(label + "-cooked-metadata", result["representation"] == 2 and result["target"] == 1
                   and result["schema"] == spec["asset_schema"] and result["kind"] ==
                   ("material" if spec["profile"].startswith("material") else "texture"))
        # Inspect is a separate native validator; it must describe exactly the final product.
        inspected = self.invoke("inspect", output, result["leaf"])
        self.check(label + "-inspect-agrees", all(result[k] == v for k, v in inspected.items()))
        return result

    def rejected_recipe(self, label: str, raw: bytes, *, output: Path | None = None) -> None:
        output = output or self.root / "products"
        (self.root / "recipes" / (label + ".json")).write_bytes(raw)
        before = snapshot(output)
        self.invoke("recipe", self.root / "recipes", label + ".json", self.root / "sources", output, success=False)
        self.check(label + "-output-unchanged", snapshot(output) == before)
        self.check(label + "-recipe-retained", (self.root / "recipes" / (label + ".json")).read_bytes() == raw)

    def run(self) -> None:
        self.report["tool_sha256_before"] = digest(self.tool.read_bytes()).hex()
        self.identity = self.invoke("--identity", identity=True)
        self.check("identity-shape", len(self.identity) == 2 and bool(re.fullmatch("[0-9a-f]{40}", self.identity[0]))
                   and bool(re.fullmatch("[0-9a-f]{64}", self.identity[1])))
        self.check("expected-source-identity", self.expected_identity is None or self.identity == self.expected_identity)
        self.report["identity"] = self.identity
        # Eight texture profiles, each checked against an independently encoded wire.
        for profile in (value for value in PROFILES if not value.startswith("material")):
            fmt = profile.split("-")[0]
            source = bmp(3, 2, GOLDEN_RGB) if fmt == "bmp" else ppm(3, 2, GOLDEN_RGB)
            leaf = profile + "." + fmt
            (self.root / "sources" / leaf).write_bytes(source)
            scalar = profile.endswith("-scalar-mips")
            encoding = 2 if scalar or profile.endswith("linear") else 1
            stage = "cook-scalar-mips" if scalar else "cook-mips" if profile.endswith("mips") else "cook"
            imported = native_wire(source, fmt, 3, 2, GOLDEN_RGB, encoding=encoding)
            cooked = native_wire(source, fmt, 3, 2, GOLDEN_RGB, encoding=encoding, stage=stage)
            spec = definition(profile, leaf)
            raw = self.create(profile, spec)
            result = self.execute(profile, raw, spec, source, imported, cooked)
            # Old direct commands remain byte-identical to recipe execution.
            direct = self.invoke("texture", self.root / "sources", leaf, self.root / "direct", IDENTITY,
                                 "linear" if encoding == 2 else "srgb")
            direct_cooked = self.invoke(stage, self.root / "direct", direct["leaf"], self.root / "direct")
            self.check(profile + "-direct-parity", direct["revision"] == result["imported_revision"]
                       and direct_cooked["revision"] == result["revision"]
                       and (self.root / "direct" / direct["leaf"]).read_bytes() == imported
                       and (self.root / "direct" / direct_cooked["leaf"]).read_bytes() == cooked)
            self.products[profile] = dict(spec=spec, raw=raw, source=source, imported=imported, cooked=cooked, result=result)
        for profile, golden, recipe_golden in (
                ("ppm-scalar-mips", "8103d647b0c8207f0cb1ad5e2bf8c755a15c64afa56f1164dd90194b91ae2da2", "f651cfc3991d4fca0c64c656ea3ff4194dd8db0cd7c714f46b4e6070802f9371"),
                ("bmp-scalar-mips", "962f6d601707caa9031a409b65b5615425e4a1937a2ac15602c73ecdc58fca7a", "4e08f356d0f730d5213dfec527b8bfd868fef6d2e5e457bb2c201439685a7d3e")):
            item = self.products[profile]
            legacy = self.products[profile.split("-")[0] + "-linear"]
            self.check(profile + "-import-provenance-preserved", item["imported"] == legacy["imported"])
            self.check(profile + "-fixed-native-golden", item["result"]["revision"] == golden)
            self.check(profile + "-fixed-recipe-golden", digest(item["raw"]).hex() == recipe_golden)
            for field in ("asset_schema", "import_settings", "settings", "cooker"):
                bad = dict(item["spec"])
                bad[field] = 2 if field == "asset_schema" else "0" * 64
                self.rejected_recipe(profile + "-tuple-" + field.replace("_", "-"), canonical(bad))
        # Recipe execution also exercises odd dimensions and top-down BMP rows.
        for profile in ("ppm-scalar-mips", "bmp-scalar-mips"):
            fmt = profile.split("-")[0]
            rgb = rgb_fixture(5, 3)
            source = bmp(5, 3, rgb, top_down=True, size_image_zero=True) if fmt == "bmp" else ppm(5, 3, rgb)
            label = profile + "-odd"
            leaf = label + "." + fmt
            (self.root / "sources" / leaf).write_bytes(source)
            spec = definition(profile, leaf)
            raw = self.create(label, spec)
            imported = native_wire(source, fmt, 5, 3, rgb, encoding=2)
            cooked = native_wire(source, fmt, 5, 3, rgb, encoding=2, stage="cook-scalar-mips")
            self.execute(label, raw, spec, source, imported, cooked)
        for profile, texture_profile in (("material", "ppm-srgb"), ("material-mips", "bmp-srgb-mips"),
                                         ("material-scalar-mips", "bmp-scalar-mips")):
            pin = (IDENTITY, self.products[texture_profile]["result"]["revision"])
            source = material_source(profile, pin)
            leaf = profile + ".json"
            (self.root / "sources" / leaf).write_bytes(source)
            spec = definition(profile, leaf, MATERIAL_ID, pin)
            imported = material_wire(source, profile, pin, cooked=False)
            cooked = material_wire(source, profile, pin, cooked=True)
            raw = self.create(profile, spec)
            result = self.execute(profile, raw, spec, source, imported, cooked)
            direct = self.invoke("material", self.root / "sources", leaf, self.root / "direct", MATERIAL_ID)
            direct_cooked = self.invoke("cook", self.root / "direct", direct["leaf"], self.root / "direct")
            self.check(profile + "-direct-parity", direct["revision"] == result["imported_revision"]
                       and direct_cooked["revision"] == result["revision"]
                       and (self.root / "direct" / direct_cooked["leaf"]).read_bytes() == cooked)
            self.products[profile] = dict(spec=spec, raw=raw, source=source, imported=imported, cooked=cooked, result=result)
        # A non-textured legacy material remains a supported distinct dependency contract.
        spec = definition("material", "plain.json", MATERIAL_ID)
        source = material_source("material", None)
        (self.root / "sources" / "plain.json").write_bytes(source)
        raw = self.create("plain", spec)
        self.execute("plain", raw, spec, source, material_wire(source, "material", None, cooked=False),
                     material_wire(source, "material", None, cooked=True))
        original = self.products["ppm-srgb-mips"]
        # Raw ordering and whitespace identify the invocation, not native provenance.
        reordered = (json.dumps(dict(reversed(list(original["spec"].items()))), indent=2) + "\n").encode()
        (self.root / "recipes" / "reordered.json").write_bytes(reordered)
        reordered_result = self.execute("reordered", reordered, original["spec"], original["source"], original["imported"], original["cooked"])
        self.check("raw-recipe-revision-distinct", reordered_result["recipe_revision"] != original["result"]["recipe_revision"])
        exact = original["raw"] + b" " * (4096 - len(original["raw"]))
        (self.root / "recipes" / "maximum.json").write_bytes(exact)
        self.execute("maximum", exact, original["spec"], original["source"], original["imported"], original["cooked"])
        self.rejected_recipe("oversized", exact + b" ")
        # Every compiled tuple component is admitted independently, not merely the profile name.
        for field in ("schema", "asset_container", "asset_schema", "target", "import_settings", "settings", "importer", "cooker", "dependencies"):
            bad = dict(original["spec"])
            bad[field] = 99 if isinstance(bad[field], int) else "0" * 64
            self.rejected_recipe("tuple-" + field.replace("_", "-"), canonical(bad))
        for field in original["spec"]:
            without = dict(original["spec"]); del without[field]
            self.rejected_recipe("missing-" + field.replace("_", "-"), canonical(without))
            duplicated = original["raw"].rstrip()[:-1] + b"," + canonical({field: original["spec"][field]})[1:]
            self.rejected_recipe("duplicate-" + field.replace("_", "-"), duplicated)
        malformed = {"unknown": canonical(dict(original["spec"], extra=1)), "trailing": original["raw"] + b"x",
                     "bom": b"\xef\xbb\xbf" + original["raw"], "nul": original["raw"] + b"\0",
                     "escaped-source": original["raw"].replace(b'"ppm-srgb-mips.ppm"', b'"\\u0070pm-srgb-mips.ppm"'),
                     "fraction-schema": original["raw"].replace(b'"schema":1', b'"schema":1.0'),
                     "signed-target": original["raw"].replace(b'"target":1', b'"target":+1')}
        for label, raw in malformed.items():
            self.rejected_recipe(label, raw)
        # Explicit recipe source formats do not silently use direct signature dispatch.
        for profile, foreign in (("ppm-srgb", bmp(3, 2, GOLDEN_RGB)), ("bmp-srgb", ppm(3, 2, GOLDEN_RGB)),
                                 ("ppm-scalar-mips", bmp(3, 2, GOLDEN_RGB)), ("bmp-scalar-mips", ppm(3, 2, GOLDEN_RGB))):
            item = self.products[profile]
            (self.root / "sources" / item["spec"]["source"]).write_bytes(foreign)
            self.rejected_recipe("wrong-format-" + profile, item["raw"])
            (self.root / "sources" / item["spec"]["source"]).write_bytes(item["source"])
        # Recipe/source pin and schema agreement is checked before any publication.
        item = self.products["material-mips"]
        bad_pin = (IDENTITY, self.products["ppm-srgb"]["result"]["revision"])
        for label, source in (("source-pin-mismatch", material_source("material-mips", bad_pin)),
                              ("source-profile-mismatch", material_source("material", (item["spec"]["texture_asset"], item["spec"]["texture_revision"]))),
                              ("source-material-invalid", b"{}")):
            (self.root / "sources" / item["spec"]["source"]).write_bytes(source)
            self.rejected_recipe(label, item["raw"])
        (self.root / "sources" / item["spec"]["source"]).write_bytes(item["source"])
        self.rejected_recipe("missing-dependency", item["raw"], output=self.root / "missing-dependency")
        dependency_leaf = item["spec"]["texture_revision"] + ".rasset"
        (self.root / "wrong-dependency" / dependency_leaf).write_bytes(self.products["ppm-srgb"]["cooked"])
        self.rejected_recipe("wrong-dependency-revision", item["raw"], output=self.root / "wrong-dependency")
        # Syntactically matching pins can still refer to incompatible native profiles.
        for texture_profile in ("ppm-srgb", "ppm-linear", "bmp-linear", "ppm-scalar-mips", "bmp-scalar-mips"):
            pin = (IDENTITY, self.products[texture_profile]["result"]["revision"])
            spec = definition("material-mips", "dependency-invalid.json", MATERIAL_ID, pin)
            (self.root / "sources" / spec["source"]).write_bytes(material_source("material-mips", pin))
            self.rejected_recipe("dependency-profile-" + texture_profile, canonical(spec))
        scalar_item = self.products["material-scalar-mips"]
        scalar_pin = (scalar_item["spec"]["texture_asset"], scalar_item["spec"]["texture_revision"])
        for texture_profile in ("ppm-srgb", "ppm-srgb-mips", "ppm-linear", "bmp-linear"):
            pin = (IDENTITY, self.products[texture_profile]["result"]["revision"])
            spec = definition("material-scalar-mips", "scalar-invalid.json", MATERIAL_ID, pin)
            (self.root / "sources" / spec["source"]).write_bytes(material_source("material-scalar-mips", pin))
            self.rejected_recipe("scalar-dependency-" + texture_profile, canonical(spec))
        (self.root / "sources" / scalar_item["spec"]["source"]).write_bytes(material_source("material-mips", scalar_pin))
        self.rejected_recipe("scalar-source-role-mismatch", scalar_item["raw"])
        (self.root / "sources" / scalar_item["spec"]["source"]).write_bytes(scalar_item["source"])
        self.rejected_recipe("scalar-missing-dependency", scalar_item["raw"], output=self.root / "missing-dependency")
        imported_pin = (IDENTITY, self.products["bmp-srgb-mips"]["result"]["imported_revision"])
        spec = definition("material-mips", "dependency-imported.json", MATERIAL_ID, imported_pin)
        (self.root / "sources" / spec["source"]).write_bytes(material_source("material-mips", imported_pin))
        self.rejected_recipe("dependency-imported", canonical(spec))
        before = snapshot(self.root / "recipes")
        for profile in ("png-srgb", "bmp-linear-mips", "PPM-SRGB"):
            self.invoke("create-recipe", self.root / "recipes", "unsupported.json", IDENTITY, "source.ppm", profile, success=False)
        self.invoke("create-recipe", self.root / "recipes", "unpinned.json", MATERIAL_ID, "source.json", "material-mips", success=False)
        self.invoke("create-recipe", self.root / "recipes", "unpinned-scalar.json", MATERIAL_ID, "source.json", "material-scalar-mips", success=False)
        self.invoke("create-recipe", self.root / "recipes", "path.json", IDENTITY, "../source.ppm", "ppm-srgb", success=False)
        self.invoke("create-recipe", self.root / "recipes", "zero.json", "0" * 32, "source.ppm", "ppm-srgb", success=False)
        self.check("invalid-creation-preserves-recipes", snapshot(self.root / "recipes") == before)
        self.invoke("create-recipe", self.root / "recipes", "ppm-srgb.json", IDENTITY, "other.ppm", "ppm-srgb", success=False)
        self.check("immutable-recipe-conflict-preserved", snapshot(self.root / "recipes") == before)
        # Reimport changes revisions under the same asset ID and preserves all prior leaves.
        for profile in ("ppm-srgb-mips", "bmp-srgb", "ppm-scalar-mips", "bmp-scalar-mips"):
            item = self.products[profile];fmt = profile.split("-")[0]
            changed_rgb = bytes([255, 64, 3]) + GOLDEN_RGB[3:]
            changed = bmp(3, 2, changed_rgb) if fmt == "bmp" else ppm(3, 2, changed_rgb)
            (self.root / "sources" / item["spec"]["source"]).write_bytes(changed)
            before_products = snapshot(self.root / "products")
            scalar = profile.endswith("-scalar-mips")
            encoding = 2 if scalar else 1
            stage = "cook-scalar-mips" if scalar else "cook-mips" if profile.endswith("mips") else "cook"
            imported = native_wire(changed, fmt, 3, 2, changed_rgb, encoding=encoding)
            cooked = native_wire(changed, fmt, 3, 2, changed_rgb, encoding=encoding, stage=stage)
            result = self.execute(profile, item["raw"], item["spec"], changed, imported, cooked)
            after_products = snapshot(self.root / "products")
            self.check(profile + "-reimport-retains-prior-products", result["id"] == IDENTITY
                       and result["revision"] != item["result"]["revision"]
                       and all(after_products.get(k) == v for k, v in before_products.items()))
            self.execute(profile, item["raw"], item["spec"], changed, imported, cooked)
        # Longstanding PPM product hashes are fixed vectors, not derived from the new adapter.
        old_source = b"P6\n# bounded header\n2 1\n255\n\n \tRGB"
        (self.root / "sources" / "legacy.ppm").write_bytes(old_source)
        spec = definition("ppm-srgb", "legacy.ppm")
        raw = self.create("legacy", spec)
        old_imported = native_wire(old_source, "ppm", 2, 1, b"\n \tRGB")
        old_cooked = native_wire(old_source, "ppm", 2, 1, b"\n \tRGB", stage="cook")
        self.check("legacy-fixed-vectors", digest(old_imported).hex() == "94e699afb8d993e35435c8473a23ad6df55fe1cc081f6202e8b0a2a2c15a293e"
                   and digest(old_cooked).hex() == "1126bcc83615edc6375fa6dad4c1f463adb874095cb8aaf83865902cf1bfbc89")
        self.execute("legacy", raw, spec, old_source, old_imported, old_cooked)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--expected-source-commit")
    parser.add_argument("--expected-authored-input-sha256")
    args = parser.parse_args()
    if bool(args.expected_source_commit) != bool(args.expected_authored_input_sha256):
        parser.error("Both expected identity fields are required together")
    expected = [args.expected_source_commit, args.expected_authored_input_sha256] if args.expected_source_commit else None
    inherited = os.environ.get("RACE_EVIDENCE_RUN_DIR")
    root = evidence_directory("surface-recipe-native", args.output,
                              parent=Path(inherited) if inherited and args.output is None else None)
    harness = RecipeHarness(args.tool.resolve(), root, expected)
    try:
        harness.run()
        harness.report.update(passed=True, state="complete")
    except BaseException as error:
        harness.report.update(passed=False, state="failed", error=f"{type(error).__name__}: {error}")
    finally:
        try:
            harness.report["tool_sha256_after"] = digest(harness.tool.read_bytes()).hex()
            harness.check("tool-unchanged", harness.report.get("tool_sha256_before") == harness.report["tool_sha256_after"])
        except BaseException as error:
            harness.report.update(passed=False, state="failed", hash_error=f"{type(error).__name__}: {error}")
        harness.report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        harness.save()
    print(json.dumps(dict(passed=harness.report["passed"], checks=len(harness.report["checks"]),
                          commands=len(harness.report["commands"]), receipt=str(root / "receipt.json"))))
    return 0 if harness.report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
