"""Host-only checkpoint path regression; never imports the native workflow."""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from output_paths import evidence_directory
from source_manifest import file_sha256
from verify_desktop_surfaces import native_guard

SCRIPT = ROOT / "scripts/test_mesh_checkpoint.py"


def load_helper(check):
    tree = ast.parse(SCRIPT.read_text(encoding="utf-8"))
    helpers = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "host_io_path"]
    check("one isolated helper without executable decorators/defaults",
          len(helpers) == 1 and not helpers[0].decorator_list and not helpers[0].args.defaults
          and not any(helpers[0].args.kw_defaults))
    snapshots = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "snapshot"]
    check("one isolated fixture snapshot without executable decorators/defaults",
          len(snapshots) == 1 and not snapshots[0].decorator_list and not snapshots[0].args.defaults
          and not any(snapshots[0].args.kw_defaults))
    namespace = {"Path": Path, "os": os, "sha": lambda data: hashlib.sha256(data).hexdigest()}
    exec(compile(ast.Module(body=helpers + snapshots, type_ignores=[]), str(SCRIPT), "exec"), namespace)
    clean = [n for n in ast.walk(tree) if isinstance(n, ast.Assign)
             and any(isinstance(t, ast.Name) and t.id == "clean" for t in n.targets)]
    check("native clean directory remains an ordinary root-relative path",
          len(clean) == 1 and isinstance(clean[0].value, ast.BinOp)
          and isinstance(clean[0].value.left, ast.Name) and clean[0].value.left.id == "root"
          and isinstance(clean[0].value.op, ast.Div)
          and isinstance(clean[0].value.right, ast.Constant)
          and clean[0].value.right.value == "clean-checkpoint")
    collision = [n for n in ast.walk(tree) if isinstance(n, ast.Call)
                 and isinstance(n.func, ast.Name) and n.func.id == "watch"
                 and n.args and isinstance(n.args[0], ast.Constant) and n.args[0].value == "collision"]
    check("collision tool arguments keep ordinary clean directory and leaf",
          len(collision) == 1 and isinstance(collision[0].args[2], ast.Name)
          and collision[0].args[2].id == "clean" and isinstance(collision[0].args[3], ast.Name)
          and collision[0].args[3].id == "leaf")
    return namespace["host_io_path"], namespace["snapshot"]


def path_cases(out, host_io_path, check, receipt):
    raw = b'{"schema":1,"fixture":"host-only"}\n'
    leaf = hashlib.sha256(raw).hexdigest() + ".json"
    for units in (261, 320):
        ordinary = out / "mesh-checkpoint-workflow" / "\u03bb-" / "clean-checkpoint" / leaf
        padding = units - len(str(ordinary).encode("utf-16-le")) // 2
        check("fixture can retain requested path length " + str(units), padding >= 0)
        ordinary = out / "mesh-checkpoint-workflow" / ("\u03bb-" + "x" * padding) / "clean-checkpoint" / leaf
        original = str(ordinary)
        host = host_io_path(ordinary)
        check("ordinary path stays exact and exceeds 260 units " + str(units),
              str(ordinary) == original and len(original.encode("utf-16-le")) // 2 == units
              and not original.startswith("\\\\?\\"))
        check("host path adds only the extended namespace " + str(units),
              str(host) == "\\\\?\\" + original and host_io_path(host) == host)
        host.parent.mkdir(parents=True)
        host.write_bytes(raw)
        check("exact long-file write/read " + str(units), host.read_bytes() == raw)
        check("long-file enumeration/read " + str(units),
              {p.name: p.read_bytes() for p in host.parent.iterdir() if p.is_file()} == {leaf: raw})
        conflict = raw + b" "
        host.write_bytes(conflict)
        check("long-file conflict bytes preserved " + str(units), host.read_bytes() == conflict)
        host.write_bytes(raw)
        check("long-file original bytes restored " + str(units), host.read_bytes() == raw)
        receipt["paths"].append({"ordinary_path": original, "host_io_path": str(host),
                                 "ordinary_utf16_units": units, "leaf": leaf,
                                 "bytes": len(raw), "sha256": file_sha256(host),
                                 "intended_tool_arguments": [str(ordinary.parent), leaf],
                                 "tool_launched": False})
    unc = Path("\\\\inert-server\\share\\\u03bb\\checkpoint.json")
    check("UNC conversion is lexical without share access",
          str(host_io_path(unc)) == "\\\\?\\UNC\\inert-server\\share\\\u03bb\\checkpoint.json")
    for relative in (Path("relative.json"), Path("C:relative.json")):
        try:
            host_io_path(relative)
        except ValueError:
            check("relative Windows path refused " + str(relative), True)
        else:
            check("relative Windows path refused " + str(relative), False)


def product_cases(out, host_io_path, snapshot, check, receipt):
    # Exercise the workflow's real directory snapshot on a cooked-product-shaped
    # path. Testing the path helper alone missed this unconverted call site.
    raw = b"RACEAST1" + bytes(436)
    digest = hashlib.sha256(raw).hexdigest()
    leaf = digest + ".rasset"
    for units in (261, 320):
        ordinary = out / "products-workflow" / "\u03bb-" / "products" / leaf
        padding = units - len(str(ordinary).encode("utf-16-le")) // 2
        check("product fixture retains requested path length " + str(units), padding >= 0)
        ordinary = out / "products-workflow" / ("\u03bb-" + "x" * padding) / "products" / leaf
        host = host_io_path(ordinary)
        host.parent.mkdir(parents=True)
        host.write_bytes(raw)
        check("ordinary product argument is unchanged " + str(units),
              len(str(ordinary).encode("utf-16-le")) // 2 == units and not str(ordinary).startswith("\\\\?\\"))
        check("deep cooked-product bytes retained " + str(units), host.read_bytes() == raw)
        check("workflow snapshot reads deep cooked product " + str(units),
              snapshot(ordinary.parent) == {leaf: digest})
        receipt["paths"].append({"ordinary_path": str(ordinary), "ordinary_utf16_units": units,
                                 "bytes": len(raw), "sha256": digest, "tool_launched": False})


def binary_cases(out, host_io_path, check, receipt):
    # Evaluate the real receipt expression without importing or launching the
    # workflow. Its host reads must accept the same relative tool paths as argv.
    tree = ast.parse(SCRIPT.read_text(encoding="utf-8"))
    expressions = [k.value for n in ast.walk(tree) if isinstance(n, ast.Call)
                   and isinstance(n.func, ast.Name) and n.func.id == "dict"
                   for k in n.keywords if k.arg == "binaries"]
    check("one actual binary receipt expression", len(expressions) == 1)
    code = compile(ast.Expression(expressions[0]), str(SCRIPT), "eval")
    folder = out / "relative-tools"
    folder.mkdir()
    paths = (folder / "tool \u03bb.exe", folder / "mesh tool.exe")
    payloads = (b"inert-tool-fixture", b"inert-mesh-tool-fixture")
    for path, raw in zip(paths, payloads):
        path.write_bytes(raw)
    relative = tuple(os.path.relpath(path, Path.cwd()) for path in paths)
    dotted = tuple(os.path.join(os.path.dirname(name), ".", os.path.basename(name)) for name in relative)
    parent = tuple(str(Path(name).parent / ".." / folder.name / Path(name).name) for name in relative)
    cases = (("relative", relative), ("dot", dotted), ("parent-component", parent),
             ("mixed", (str(paths[0]), relative[1])), ("absolute", tuple(map(str, paths))))
    for label, argv in cases:
        args = argparse.Namespace(tool=argv[0], mesh_tool=argv[1])
        namespace = {"a": args, "Path": Path, "os": os, "host_io_path": host_io_path,
                     "sha": lambda data: hashlib.sha256(data).hexdigest()}
        binaries = eval(code, namespace)
        check("binary hashes match file bytes " + label,
              binaries == {name: hashlib.sha256(raw).hexdigest() for name, raw in zip(argv, payloads)})
        check("binary receipt preserves tool arguments and keys " + label,
              (args.tool, args.mesh_tool) == argv and tuple(binaries) == argv)
        receipt.setdefault("binary_paths", []).append({"case": label, "arguments": argv,
                                                        "binaries": binaries, "tool_launched": False})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    out = evidence_directory("p04-checkpoint-path-fixtures", args.output)
    receipt = {"schema": 1, "status": "checking", "checks": [], "paths": [],
               "script_sha256": file_sha256(SCRIPT), "tests_sha256": file_sha256(Path(__file__)),
               "engine_or_tool_launched": False, "target_module_imported": False}
    def check(name, value):
        receipt["checks"].append({"check": name, "passed": bool(value)})
        if not value:
            raise AssertionError(name)
    try:
        receipt["guard"] = native_guard(out)
        if not receipt["guard"]["passed"]:
            receipt["status"] = "held"
        else:
            check("Windows host path I/O fixture", os.name == "nt")
            host_io_path, snapshot = load_helper(check)
            path_cases(out, host_io_path, check, receipt)
            product_cases(out, host_io_path, snapshot, check, receipt)
            binary_cases(out, host_io_path, check, receipt)
            receipt["status"] = "passed"
    except BaseException as error:
        receipt.update(status="failed", error=repr(error))
    finally:
        receipt["source_unchanged"] = (file_sha256(SCRIPT) == receipt["script_sha256"]
                                       and file_sha256(Path(__file__)) == receipt["tests_sha256"])
        with (out / "tests.json").open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2, sort_keys=True)
    print(json.dumps({"status": receipt["status"], "checks": len(receipt["checks"]),
                      "error": receipt.get("error"), "receipt": str(out / "tests.json")}))
    return 0 if receipt["status"] == "passed" else 75 if receipt["status"] == "held" else 1


if __name__ == "__main__":
    raise SystemExit(main())
