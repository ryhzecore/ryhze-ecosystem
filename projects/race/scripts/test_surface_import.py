"""Independent retained CLI oracles for bounded BMP24 and legacy PPM import."""
from __future__ import annotations

import argparse
from bisect import bisect_right
from datetime import datetime, timezone
from decimal import Decimal, localcontext, ROUND_CEILING, ROUND_HALF_UP
from fractions import Fraction
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

from output_paths import evidence_directory


IDENTITY = "00000000000000040000000000000008"
GOLDEN_RGB = bytes([10, 32, 9, 255, 0, 0, 0, 255, 0,
                    0, 0, 255, 128, 128, 128, 255, 255, 0])
MIP_SETTINGS = ("race-rgba8-mips-1:top-left;opaque-srgb;q24-decode-nearest-half-up;"
                "area-overlap-integer;floor-half-to-1;recursive-rgba8;mean-q24-half-up;"
                "encode-midpoint-ceil")
MIP_COOKER = "race-rgba8-portable-cpu-mips-cooker-1:srgb-q24-lut-1"
SCALAR_MIP_SETTINGS = ("race-rgba8-scalar-mips-1:top-left;opaque-linear-data;independent-rgb;"
                       "area-overlap-integer;floor-half-to-1;recursive-rgba8;mean-half-up")
SCALAR_MIP_COOKER = "race-rgba8-portable-cpu-scalar-mips-cooker-1"
SCALAR_MATERIAL_SETTINGS = "race-opaque-diffuse-3:linear-rgb;linear-clamp-mips;complete-cooked-linear-scalar-texture;diffuse-reflectance-r"
SCALAR_MATERIAL_IMPORTER = "race-material-json-importer-3"
SCALAR_MATERIAL_COOKER = "race-opaque-diffuse-portable-cpu-cooker-3:diffuse-reflectance-r;linear-clamp-mips"



def digest(value: bytes) -> bytes:
    return hashlib.sha256(value).digest()


def rgb_fixture(width: int, height: int = 2) -> bytes:
    if (width, height) == (3, 2):
        return GOLDEN_RGB
    return bytes((10 + 31 * x + 73 * y + 47 * channel) % 256
                 for y in range(height) for x in range(width) for channel in range(3))


def ppm(width: int, height: int, rgb: bytes) -> bytes:
    return f"P6\n{width} {height}\n255\n".encode("ascii") + rgb


def bmp(width: int, height: int, rgb: bytes, *, top_down: bool = False,
        size_image_zero: bool = False, padding: int = 0xA5) -> bytes:
    """Construct a file independently of the native decoder, including row bytes."""
    row_bytes = width * 3
    stride = (row_bytes + 3) // 4 * 4
    raster = bytearray()
    for y in (range(height) if top_down else reversed(range(height))):
        for x in range(width):
            red, green, blue = rgb[(y * width + x) * 3:(y * width + x + 1) * 3]
            raster.extend((blue, green, red))
        raster.extend(bytes([padding]) * (stride - row_bytes))
    return (struct.pack("<2sIHHI", b"BM", 54 + len(raster), 0, 0, 54)
            + struct.pack("<IiiHHIIiiII", 40, width, -height if top_down else height,
                          1, 24, 0, 0 if size_image_zero else len(raster), 0, 0, 0, 0)
            + raster)


def rgba(rgb: bytes) -> bytes:
    return b"".join(rgb[index:index + 3] + b"\xff" for index in range(0, len(rgb), 3))


@lru_cache(maxsize=1)
def conversion_tables() -> tuple[list[int], list[int]]:
    # Independent Decimal evaluation of the declared Q24 wire-cooking profile.
    # No production table, implementation helper, or native output is read.
    with localcontext() as context:
        context.prec = 80

        def linear(value: Decimal) -> Decimal:
            return (value / Decimal("12.92") if value <= Decimal("0.04045")
                    else ((value + Decimal("0.055")) / Decimal("1.055")) ** Decimal("2.4"))

        scale = Decimal(1 << 24)
        decoded = [int((linear(Decimal(code) / 255) * scale).to_integral_value(rounding=ROUND_HALF_UP))
                   for code in range(256)]
        thresholds = [int((linear((Decimal(code) + Decimal("0.5")) / 255) * scale)
                          .to_integral_value(rounding=ROUND_CEILING)) for code in range(255)]
    return decoded, thresholds


def mip_chain(width: int, height: int, rgb: bytes, *, scalar: bool = False) -> list[bytes]:
    """Exact rational pixel-box intersections; independently quantized levels.

    Scalar channels average stored linear bytes independently. They do not use
    the sRGB transfer function or interpret RGB triples as normal vectors.
    """
    decoded, thresholds = (list(range(256)), []) if scalar else conversion_tables()
    levels = [rgba(rgb)]
    while width != 1 or height != 1:
        next_width, next_height = max(1, width // 2), max(1, height // 2)
        reduced = bytearray()
        for y in range(next_height):
            top, bottom = Fraction(y * height, next_height), Fraction((y + 1) * height, next_height)
            for x in range(next_width):
                left, right = Fraction(x * width, next_width), Fraction((x + 1) * width, next_width)
                sums = [Fraction(0)] * 3
                for sy in range(top.numerator // top.denominator, -(-bottom.numerator // bottom.denominator)):
                    for sx in range(left.numerator // left.denominator, -(-right.numerator // right.denominator)):
                        area = (min(right, sx + 1) - max(left, sx)) * (min(bottom, sy + 1) - max(top, sy))
                        for channel in range(3):
                            sums[channel] += decoded[levels[-1][(sy * width + sx) * 4 + channel]] * area
                area = (right - left) * (bottom - top)
                for value in sums:
                    mean = value / area
                    rounded = (2 * mean.numerator + mean.denominator) // (2 * mean.denominator)
                    reduced.append(rounded if scalar else bisect_right(thresholds, rounded))
                reduced.append(255)
        levels.append(bytes(reduced))
        width, height = next_width, next_height
    return levels


def native_wire(source: bytes, profile: str, width: int, height: int, rgb: bytes,
                *, encoding: int = 1, stage: str = "import") -> bytes:
    """Encode the documented envelope independently with struct/hashlib."""
    scalar = stage == "cook-scalar-mips"
    is_mips = scalar or stage == "cook-mips"
    if scalar and encoding != 2:
        raise ValueError("Scalar mip oracle requires explicit LinearData encoding")
    payloads = mip_chain(width, height, rgb, scalar=scalar) if is_mips else [rgba(rgb)]
    payload = b"".join(payloads)
    header = bytearray(256)
    header[:8] = b"RACEAST1"
    struct.pack_into("<4I", header, 8, 1, 256, 2, 3 if scalar else 2 if is_mips else 1)
    struct.pack_into("<2I", header, 24, 1 if stage == "import" else 2, 0 if stage == "import" else 1)
    struct.pack_into("<6Q", header, 32, 288 + len(payload), 256, len(payload), 0, 4, 8)
    header[80:112] = digest(source)
    settings = SCALAR_MIP_SETTINGS if scalar else MIP_SETTINGS if is_mips else (
        f"race-{profile}-1:rgba8;top-left;{'srgb' if encoding == 1 else 'linear-data'};opaque")
    header[112:144] = digest(settings.encode("ascii"))
    importer = "race-bmp-bgr24-info40-importer-1" if profile == "bmp" else "race-ppm-p6-importer-1"
    header[144:176] = digest(importer.encode("ascii"))
    if stage != "import":
        header[176:208] = digest((SCALAR_MIP_COOKER if scalar else MIP_COOKER if is_mips else "race-rgba8-portable-cpu-cooker-1").encode("ascii"))
    header[208:240] = digest(b"")
    struct.pack_into("<4I", header, 240, width, height, encoding, len(payloads))
    body = bytes(header) + payload
    return body + digest(body)


def scalar_material_wire(source: bytes, pin: tuple[str, str], *, cooked: bool,
                         identity: str = "00000000000000050000000000000009") -> bytes:
    """Independent schema3 material wire; role2 is separate from sampler2."""
    key = struct.pack("<QQ", int(pin[0][:16], 16), int(pin[0][16:], 16)) + bytes.fromhex(pin[1])
    header = bytearray(256)
    header[:8] = b"RACEAST1"
    struct.pack_into("<6I", header, 8, 1, 256, 3, 3, 2 if cooked else 1, 1 if cooked else 0)
    struct.pack_into("<6Q", header, 32, 360, 256, 72, 0, int(identity[:16], 16), int(identity[16:], 16))
    header[80:112] = digest(source)
    header[112:144] = digest(SCALAR_MATERIAL_SETTINGS.encode())
    header[144:176] = digest(SCALAR_MATERIAL_IMPORTER.encode())
    if cooked:
        header[176:208] = digest(SCALAR_MATERIAL_COOKER.encode())
    header[208:240] = digest(key)
    struct.pack_into("<4I", header, 240, 1, 1, 2, 2)
    body = bytes(header) + struct.pack("<3d", 1, 0.25, 0.1) + key
    return body + digest(body)


def snapshot(root: Path) -> dict[str, bytes]:
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise RuntimeError("Unexpected link in native output")
        if path.is_file():
            result[path.relative_to(root).as_posix()] = path.read_bytes()
    return result


class Harness:
    def __init__(self, tool: Path, root: Path):
        self.tool, self.root = tool, root
        self.report = {"schema": 1, "passed": False, "state": "running", "checks": [], "commands": [],
                       "started_utc": datetime.now(timezone.utc).isoformat(), "output": str(root)}
        self.identity: list[str] = []
        for name in ("sources", "products", "repeat", "commands", "conflict"):
            (root / name).mkdir()
        self.save()

    def save(self) -> None:
        (self.root / "receipt.json").write_text(json.dumps(self.report, indent=2) + "\n", encoding="utf-8")

    def check(self, name: str, passed: bool, **details) -> None:
        self.report["checks"].append(dict(name=name, passed=bool(passed), **details))
        if not passed:
            raise RuntimeError(name)

    def invoke(self, *arguments, success: bool = True, identity: bool = False):
        command = [str(self.tool), *map(str, arguments)]
        location = self.root / "commands" / f"{len(self.report['commands']) + 1:03d}"
        location.mkdir()
        record = dict(command=command, expected_success=success, completed=False, output=str(location))
        self.report["commands"].append(record)
        self.save()
        run = subprocess.run(command, capture_output=True, timeout=30, check=False)
        (location / "stdout.log").write_bytes(run.stdout)
        (location / "stderr.log").write_bytes(run.stderr)
        record.update(completed=True, exit_code=run.returncode)
        self.save()
        self.check(f"command-{location.name}-exit", run.returncode == (0 if success else 1), exit_code=run.returncode)
        if not success:
            self.check(f"command-{location.name}-native-rejection", run.stderr.startswith(b"surface_error code="))
            return None
        if identity:
            return run.stdout.decode("ascii").strip().split()
        value = json.loads(run.stdout.decode("utf-8"))
        self.check(f"command-{location.name}-identity", [value.get("source_commit"), value.get("authored_sha256")] == self.identity)
        return value

    def verify(self, label: str, record: dict, expected: bytes, output: Path) -> bytes:
        revision = digest(expected).hex()
        self.check(label + "-revision-leaf", record["revision"] == revision and record["leaf"] == revision + ".rasset")
        actual = (output / record["leaf"]).read_bytes()
        self.check(label + "-entire-wire", actual == expected, expected_sha256=revision,
                   actual_sha256=digest(actual).hex(), bytes=len(actual))
        metadata = struct.unpack_from("<4I", expected, 240)
        self.check(label + "-report", record["format"] == "RACEAST1" and record["kind"] == "texture"
                   and record["id"] == IDENTITY and record["schema"] == struct.unpack_from("<I", expected, 20)[0]
                   and (record["representation"], record["target"]) == struct.unpack_from("<2I", expected, 24)
                   and (record["width"], record["height"], record["encoding"], record["mip_count"]) == metadata
                   and record["texel_bytes"] == len(expected) - 288
                   and record["profile"] == ("opaque-scalar-mips" if record["schema"] == 3 else "opaque-srgb-mips" if record["schema"] == 2 else "rgba8-mip0")
                   and record["sampling"] == "none")
        return actual

    def workflow(self, label: str, source: bytes, profile: str, width: int, height: int, rgb: bytes,
                 *, suffix: str | None = None, encoding: int = 1) -> dict[str, tuple[dict, bytes]]:
        leaf = label + "." + (suffix or profile)
        (self.root / "sources" / leaf).write_bytes(source)
        output = self.root / "products"
        imported = self.invoke("texture", self.root / "sources", leaf, output, IDENTITY,
                               "srgb" if encoding == 1 else "linear")
        products = {}
        for stage in ("import", "cook", "cook-mips") if encoding == 1 else ("import", "cook", "cook-scalar-mips"):
            record = imported if stage == "import" else self.invoke(stage, output, imported["leaf"], output)
            expected = native_wire(source, profile, width, height, rgb, encoding=encoding, stage=stage)
            products[stage] = record, self.verify(label + "-" + stage, record, expected, output)
        return products

    def reject_source(self, label: str, source: bytes, *, leaf: str | None = None) -> None:
        leaf = leaf or label + ".bmp"
        (self.root / "sources" / leaf).write_bytes(source)
        before = snapshot(self.root / "products")
        self.invoke("texture", self.root / "sources", leaf, self.root / "products", IDENTITY, "srgb", success=False)
        self.check(label + "-output-unchanged", snapshot(self.root / "products") == before)

    def scalar_material(self, scalar: dict, incompatible: list[dict]) -> None:
        output = self.root / "products"
        pin = (IDENTITY, scalar["revision"])
        definition = {"schema": 3, "model": "opaque-diffuse", "base_color": [1, 0.25, 0.1],
                      "base_color_texture": {"asset": pin[0], "revision": pin[1]},
                      "sampling": "linear-clamp-mips", "texture_role": "diffuse-reflectance-r"}
        source = (json.dumps(definition, separators=(",", ":")) + "\n").encode()
        (self.root / "sources" / "scalar-material.json").write_bytes(source)
        imported = self.invoke("material", self.root / "sources", "scalar-material.json", output,
                               "00000000000000050000000000000009")
        cooked = self.invoke("cook", output, imported["leaf"], output)
        for stage, record in ((False, imported), (True, cooked)):
            expected = scalar_material_wire(source, pin, cooked=stage)
            self.check("scalar-material-wire-" + str(stage), (output / record["leaf"]).read_bytes() == expected
                       and record["revision"] == digest(expected).hex() and record["schema"] == 3
                       and record["kind"] == "material" and record["sampling"] == "linear-clamp-mips")
            self.check("scalar-material-inspect-" + str(stage), self.invoke("inspect", output, record["leaf"]) == record)
        self.check("scalar-material-repeat", self.invoke("cook", output, imported["leaf"], output) == cooked)
        for label, candidate in enumerate(incompatible):
            value = dict(definition, base_color_texture={"asset": IDENTITY, "revision": candidate["revision"]})
            raw = (json.dumps(value, separators=(",", ":")) + "\n").encode()
            (self.root / "sources" / "wrong-material.json").write_bytes(raw)
            wrong = self.invoke("material", self.root / "sources", "wrong-material.json", output,
                                "00000000000000050000000000000009")
            before = snapshot(output)
            self.invoke("cook", output, wrong["leaf"], output, success=False)
            self.check("scalar-material-wrong-texture-" + str(label), snapshot(output) == before)
        for label, changes in (("old-schema", {"schema": 2}), ("missing-role", {}),
                               ("wrong-role", {"texture_role": "color"}),
                               ("wrong-channel", {"texture_role": "diffuse-reflectance-g"}),
                               ("unpinned", {"base_color_texture": None}),
                               ("mip0", {"sampling": "linear-clamp-mip0"})):
            value = dict(definition, **changes)
            if label == "missing-role":
                del value["texture_role"]
            (self.root / "sources" / "wrong-material.json").write_bytes(json.dumps(value).encode())
            before = snapshot(output)
            self.invoke("material", self.root / "sources", "wrong-material.json", output,
                        "00000000000000050000000000000009", success=False)
            self.check("scalar-material-source-rejected-" + label, snapshot(output) == before)
        valid = (output / cooked["leaf"]).read_bytes()
        for offset, values in ((20, (1, 2, 4)), (248, (0, 1, 3)), (252, (0, 1, 3, 0xFFFFFFFF))):
            for value in values:
                changed = bytearray(valid[:-32]);struct.pack_into("<I", changed, offset, value)
                malformed = bytes(changed) + digest(changed)
                leaf = digest(malformed).hex() + ".rasset"
                (self.root / "conflict" / leaf).write_bytes(malformed)
                self.invoke("inspect", self.root / "conflict", leaf, success=False)
        self.check("scalar-material-prior-published-preserved", (output / cooked["leaf"]).read_bytes() == valid)

    def run(self) -> None:
        self.report["tool"] = str(self.tool)
        self.report["tool_sha256_before"] = digest(self.tool.read_bytes()).hex()
        self.identity = self.invoke("--identity", identity=True)
        self.check("tool-build-identity", len(self.identity) == 2
                   and re.fullmatch(r"[0-9a-f]{40}", self.identity[0]) is not None
                   and re.fullmatch(r"[0-9a-f]{64}", self.identity[1]) is not None)
        self.report["build_identity"] = self.identity
        # Preserve the pre-BMP PPM golden, including header comment and binary whitespace.
        old_source = b"P6\n# bounded header\n2 1\n255\n\n \tRGB"
        old = self.workflow("legacy-golden", old_source, "ppm", 2, 1, b"\n \tRGB")
        self.check("ppm-imported-golden-unchanged", old["import"][0]["revision"] == "94e699afb8d993e35435c8473a23ad6df55fe1cc081f6202e8b0a2a2c15a293e")
        self.check("ppm-cooked-golden-unchanged", old["cook"][0]["revision"] == "1126bcc83615edc6375fa6dad4c1f463adb874095cb8aaf83865902cf1bfbc89")
        primary = None
        for width in range(1, 5):
            rgb = rgb_fixture(width)
            reference = self.workflow(f"ppm-width-{width}", ppm(width, 2, rgb), "ppm", width, 2, rgb)
            for top_down in (False, True):
                for zero_size in (False, True):
                    label = f"bmp-width-{width}-{'top' if top_down else 'bottom'}-{'zero' if zero_size else 'exact'}"
                    source = bmp(width, 2, rgb, top_down=top_down, size_image_zero=zero_size)
                    actual = self.workflow(label, source, "bmp", width, 2, rgb)
                    for stage in actual:
                        self.check(label + "-" + stage + "-ppm-pixels", actual[stage][1][256:-32] == reference[stage][1][256:-32])
                        self.check(label + "-" + stage + "-distinct-provenance", actual[stage][1] != reference[stage][1])
                    if width == 3 and not top_down and not zero_size:
                        primary = actual
        self.check("bmp-imported-golden", primary["import"][0]["revision"] == "28fa59931cc4742c089b4dffd976b942fee683f21697c76b0513a762547bfc15")
        self.check("bmp-cooked-golden", primary["cook"][0]["revision"] == "705aa4b2d6fa94634929b4d7da278c9f0236c8af8b2a2d82d53ae0c441e00957")
        self.check("bmp-schema2-mips-golden-unchanged", primary["cook-mips"][0]["revision"] == "88043bb29de5ee5ac3008cb983a2ae2538656099c7f0fde74e9e8ad3ecdad92d")
        source = bmp(3, 2, GOLDEN_RGB)
        renamed = self.workflow("bmp-renamed", source, "bmp", 3, 2, GOLDEN_RGB, suffix="ppm")
        self.check("bmp-signature-dispatch", renamed == primary)
        renamed_ppm = self.workflow("ppm-renamed", old_source, "ppm", 2, 1, b"\n \tRGB", suffix="bmp")
        self.check("ppm-signature-dispatch", renamed_ppm == old)
        # Ignored BMP padding/resolution bytes remain source provenance, not texels.
        changed = bytearray(bmp(3, 2, GOLDEN_RGB, padding=0x5A))
        struct.pack_into("<ii", changed, 38, 2835, -1)
        alternate = self.workflow("bmp-padding-resolution", bytes(changed), "bmp", 3, 2, GOLDEN_RGB)
        self.check("padding-resolution-provenance", alternate["import"][0]["revision"] != primary["import"][0]["revision"]
                   and alternate["import"][1][256:-32] == primary["import"][1][256:-32])
        linear = self.workflow("bmp-linear", source, "bmp", 3, 2, GOLDEN_RGB, encoding=2)
        self.scalar_material(linear["cook-scalar-mips"][0],
                             [primary["import"][0], primary["cook"][0], primary["cook-mips"][0], linear["import"][0], linear["cook"][0]])
        before = snapshot(self.root / "products")
        self.invoke("cook-mips", self.root / "products", linear["import"][0]["leaf"], self.root / "products", success=False)
        self.check("linear-mips-rejected-without-publication", snapshot(self.root / "products") == before)
        # Scalar profiles preserve legacy LinearData imports and cook a separate
        # schema3 chain. Tiny ties and odd extents distinguish stored-byte means
        # from sRGB filtering, vector normalization and dropping edge texels.
        self.check("scalar-half-up-independent-channels", mip_chain(2, 1, bytes([0, 64, 254, 1, 65, 255]), scalar=True)[1]
                   == bytes([1, 65, 255, 255]))
        self.check("scalar-golden-last-mip", mip_chain(3, 2, GOLDEN_RGB, scalar=True)[1]
                   == bytes([108, 112, 65, 255]))
        self.check("scalar-cooked-golden", linear["cook-scalar-mips"][0]["revision"] == "962f6d601707caa9031a409b65b5615425e4a1937a2ac15602c73ecdc58fca7a")
        self.check("legacy-linear-import-cook-unchanged", linear["import"][0]["schema"] == 1 and linear["cook"][0]["schema"] == 1)
        for width, height in ((1, 1), (1, 7), (7, 1), (5, 3)):
            rgb = rgb_fixture(width, height)
            reference = self.workflow(f"scalar-ppm-{width}x{height}", ppm(width, height, rgb), "ppm", width, height, rgb, encoding=2)
            scalar_wire = reference["cook-scalar-mips"][1]
            self.check(f"scalar-{width}x{height}-opaque-chain", all(value == 255 for value in scalar_wire[259:-32:4]))
            if (width, height) == (5, 3):
                self.check("scalar-odd-recursive-chain-golden", scalar_wire[256 + width * height * 4:-32].hex() == "6c8a97ff94907bff808d89ff")
                for top_down in (False, True):
                    label = "scalar-bmp-odd-" + ("top" if top_down else "bottom")
                    source_odd = bmp(width, height, rgb, top_down=top_down, size_image_zero=top_down)
                    actual = self.workflow(label, source_odd, "bmp", width, height, rgb, encoding=2)
                    self.check(label + "-ppm-pixels", actual["cook-scalar-mips"][1][256:-32] == scalar_wire[256:-32])
                    self.check(label + "-distinct-provenance", actual["cook-scalar-mips"][1] != scalar_wire)
        tie_rgb = bytes([0, 64, 254, 1, 65, 255])
        self.workflow("scalar-half-up", ppm(2, 1, tie_rgb), "ppm", 2, 1, tie_rgb, encoding=2)
        for mode in ("inspect", "validate"):
            self.check(mode + "-native-scalar-mips", self.invoke(mode, self.root / "products", linear["cook-scalar-mips"][0]["leaf"]) == linear["cook-scalar-mips"][0])
        before = snapshot(self.root / "products")
        for product in (primary["import"], primary["cook-mips"], linear["cook"], linear["cook-scalar-mips"]):
            self.invoke("cook-scalar-mips", self.root / "products", product[0]["leaf"], self.root / "products", success=False)
        self.check("scalar-wrong-encoding-or-representation-no-publication", snapshot(self.root / "products") == before)
        for label, offset, value in (("srgb", 248, 1), ("alpha", 259, 0), ("last-alpha", len(linear["cook-scalar-mips"][1]) - 33, 0), ("mip-count", 252, 1)):
            malformed = bytearray(linear["cook-scalar-mips"][1])
            malformed[offset] = value
            malformed[-32:] = digest(malformed[:-32])
            leaf = "scalar-invalid-" + label + ".rasset"
            (self.root / "sources" / leaf).write_bytes(malformed)
            self.invoke("validate", self.root / "sources", leaf, success=False)
        for output in (self.root / "products", self.root / "repeat"):
            imported = self.invoke("texture", self.root / "sources", "bmp-linear.bmp", output, IDENTITY, "linear")
            self.check("scalar-repeat-import-" + output.name, imported == linear["import"][0])
            record = self.invoke("cook-scalar-mips", output, imported["leaf"], output)
            self.verify("scalar-repeat-cook-" + output.name, record, linear["cook-scalar-mips"][1], output)
        # Reimport into both an occupied-identical root and a fresh root; all stages deterministic.
        for output in (self.root / "products", self.root / "repeat"):
            imported = self.invoke("texture", self.root / "sources", "bmp-renamed.ppm", output, IDENTITY, "srgb")
            self.check("repeat-import-" + output.name, imported == primary["import"][0])
            for stage in ("cook", "cook-mips"):
                record = self.invoke(stage, output, imported["leaf"], output)
                self.check("repeat-" + stage + "-" + output.name, record == primary[stage][0])
                self.verify("repeat-" + stage + "-" + output.name, record, primary[stage][1], output)
        for mode in ("inspect", "validate"):
            self.check(mode + "-native-mips", self.invoke(mode, self.root / "products", primary["cook-mips"][0]["leaf"]) == primary["cook-mips"][0])
        # Malformed headers and extents are independently mutated, never native-encoded.
        for end in (0, 1, 2, 13, 14, 18, 53, 54, len(source) - 1):
            self.reject_source(f"truncated-{end}", source[:end])
        fields = [("reserved-one", 6, "H", 1), ("reserved-two", 8, "H", 1),
                  ("file-size-small", 2, "I", len(source) - 1), ("file-size-large", 2, "I", len(source) + 1),
                  ("offset-before", 10, "I", 53), ("offset-after", 10, "I", 55), ("offset-overflow", 10, "I", 0xFFFFFFFF),
                  ("dib-short", 14, "I", 12), ("dib-extended", 14, "I", 108),
                  ("width-zero", 18, "i", 0), ("width-negative", 18, "i", -1), ("width-bound", 18, "i", 1025),
                  ("width-max", 18, "i", 0x7FFFFFFF), ("height-zero", 22, "i", 0),
                  ("height-bound", 22, "i", 1025), ("height-negative-bound", 22, "i", -1025),
                  ("height-min", 22, "i", -0x80000000), ("height-max", 22, "i", 0x7FFFFFFF),
                  ("planes-zero", 26, "H", 0), ("planes-two", 26, "H", 2), ("bpp-eight", 28, "H", 8),
                  ("bpp-thirtytwo", 28, "H", 32), ("compression-rle", 30, "I", 1),
                  ("compression-bitfields", 30, "I", 3), ("image-size-small", 34, "I", 23),
                  ("image-size-large", 34, "I", 25), ("colors-used", 46, "I", 1), ("colors-important", 50, "I", 1)]
        for label, offset, kind, value in fields:
            changed = bytearray(source)
            struct.pack_into("<" + kind, changed, offset, value)
            self.reject_source(label, bytes(changed))
        self.reject_source("signature", b"ZZ" + source[2:])
        self.reject_source("ppm-trailing", old_source + b"x", leaf="ppm-trailing.ppm")
        self.reject_source("trailing-byte", source + b"x")
        changed = bytearray(source + b"x")
        struct.pack_into("<I", changed, 2, len(changed))
        self.reject_source("trailing-declared", bytes(changed))
        changed = bytearray(source[:54] + b"\x00\x00\x00\x00" + source[54:])
        struct.pack_into("<I", changed, 2, len(changed))
        struct.pack_into("<I", changed, 10, 58)
        self.reject_source("color-table-gap", bytes(changed))
        # Existing immutable leaves cannot be replaced by a successful import.
        conflict = self.root / "conflict"
        (conflict / primary["import"][0]["leaf"]).write_bytes(b"occupied")
        before = snapshot(conflict)
        self.invoke("texture", self.root / "sources", "bmp-renamed.ppm", conflict, IDENTITY, "srgb", success=False)
        self.check("occupied-leaf-preserved", snapshot(conflict) == before)
        before = snapshot(self.root / "products")
        for leaf in ("../bmp-renamed.ppm", "BMP-RENAMED.PPM", "sub/bmp-renamed.ppm", "bmp-renamed.ppm:stream"):
            self.invoke("texture", self.root / "sources", leaf, self.root / "products", IDENTITY, "srgb", success=False)
        self.check("noncanonical-input-output-unchanged", snapshot(self.root / "products") == before)
        self.report["tool_sha256_after"] = digest(self.tool.read_bytes()).hex()
        self.check("tool-unchanged", self.report["tool_sha256_before"] == self.report["tool_sha256_after"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    inherited = os.environ.get("RACE_EVIDENCE_RUN_DIR")
    root = evidence_directory("surface-import-native", arguments.output,
                              parent=Path(inherited) if inherited and arguments.output is None else None)
    harness = Harness(arguments.tool.resolve(), root)
    try:
        harness.run()
        harness.report.update(passed=True, state="complete")
    except BaseException as error:
        harness.report.update(passed=False, state="failed", error=f"{type(error).__name__}: {error}")
    finally:
        harness.report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        harness.save()
    print(json.dumps(dict(passed=harness.report["passed"], checks=len(harness.report["checks"]),
                          commands=len(harness.report["commands"]), receipt=str(root / "receipt.json"))))
    return 0 if harness.report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
