"""Host-only mesh-watch path checks; never import or launch the native workflow."""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from output_paths import evidence_directory
from source_manifest import file_sha256
from verify_desktop_surfaces import native_guard

SCRIPT = ROOT / "scripts/test_mesh_watch.py"


def load_fixture(check):
    tree = ast.parse(SCRIPT.read_text(encoding="utf-8"))
    names = {"host_io_path", "replace", "direct"}
    definitions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
    check("isolate only the three inert helper definitions",
          len(definitions) == len(names) and {n.name for n in definitions} == names
          and all(not n.decorator_list and not n.args.defaults
                  and not any(n.args.kw_defaults) for n in definitions))
    namespace = {"Path": Path, "os": os}
    exec(compile(ast.Module(body=definitions, type_ignores=[]), str(SCRIPT), "exec"), namespace)
    return tree, namespace


def one_node(nodes, label, check):
    nodes = list(nodes)
    check("one actual workflow " + label, len(nodes) == 1)
    return nodes[0]


def has_method(node, name):
    return any(isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
               and n.func.attr == name for n in ast.walk(node))


def run_assertion(node, namespace):
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(SCRIPT), "exec"), namespace)


def rejects_assertion(node, namespace):
    try:
        run_assertion(node, namespace)
    except AssertionError:
        return True
    return False


def long_path(out, label, leaf, units, check):
    path = out / label / "\u03bb-" / leaf
    padding = units - len(str(path).encode("utf-16-le")) // 2
    check(label + " admits requested path length", 0 <= padding <= 253)
    path = out / label / ("\u03bb-" + "x" * padding) / leaf
    check(label + " retains ordinary Unicode path",
          len(str(path).encode("utf-16-le")) // 2 == units and not str(path).startswith("\\\\?\\"))
    return path


def path_cases(out, namespace, check):
    host = namespace["host_io_path"]
    for units in (261, 320):
        ordinary = long_path(out, "source-" + str(units), "source.obj", units, check)
        original = str(ordinary)
        check("host conversion is exact and idempotent " + str(units),
              str(host(ordinary)) == "\\\\?\\" + original and host(host(ordinary)) == host(ordinary))
        host(ordinary.parent).mkdir(parents=True)
        host(ordinary).write_bytes(b"old source")
        namespace["replace"](ordinary, b"new source")
        check("actual replacement reads correct long-path bytes " + str(units),
              host(ordinary).read_bytes() == b"new source"
              and not host(ordinary.with_suffix(".tmp")).exists() and str(ordinary) == original)
    unc = Path("\\\\inert-server\\share\\\u03bb\\source.obj")
    check("UNC conversion is lexical only",
          str(host(unc)) == "\\\\?\\UNC\\inert-server\\share\\\u03bb\\source.obj")
    for path in (Path("relative.obj"), Path("C:relative.obj")):
        try:
            host(path)
        except ValueError:
            check("relative host path rejected " + str(path), True)
        else:
            check("relative host path rejected " + str(path), False)


def product_cases(out, tree, namespace, check, receipt):
    host = namespace["host_io_path"]
    product = one_node((n for n in ast.walk(tree) if isinstance(n, ast.Assert)
                        and has_method(n, "read_bytes")), "product assertion", check)
    empty = one_node((n for n in ast.walk(tree) if isinstance(n, ast.Assert)
                      and has_method(n, "glob")), "empty-output assertion", check)
    raw = b"RACEAST1" + bytes(436)
    digest = hashlib.sha256(raw).hexdigest()
    for units in (261, 320):
        ordinary = long_path(out, "product-" + str(units), digest + ".rasset", units, check)
        host(ordinary.parent).mkdir(parents=True)
        host(ordinary).write_bytes(raw)
        context = dict(namespace, active=SimpleNamespace(output=ordinary.parent), blob=raw,
                       hashlib=hashlib, final={"current_revision": "0" * 64})
        run_assertion(product, context)
        check("actual product assertion reads exact bytes " + str(units), True)
        host(ordinary).write_bytes(raw[:-1] + b"x")
        check("actual product assertion rejects corrupt bytes " + str(units),
              rejects_assertion(product, context))
        host(ordinary).write_bytes(raw)
        check("actual empty-output assertion detects deep product " + str(units),
              rejects_assertion(empty, context))
        vacant = ordinary.parent / "empty"
        host(vacant).mkdir()
        context["active"] = SimpleNamespace(output=vacant)
        run_assertion(empty, context)
        check("actual empty-output assertion accepts empty directory " + str(units), True)
        receipt["paths"].append({"ordinary_path": str(ordinary), "utf16_units": units,
                                 "sha256": digest, "bytes": len(raw)})


def argument_cases(out, tree, namespace, check, receipt):
    host = namespace["host_io_path"]
    source = long_path(out, "arguments", "source.obj", 320, check)
    base = source.parent
    host(base).mkdir(parents=True)
    imported, cooked = b"inert imported bytes", b"inert cooked bytes"
    products = {"import": imported, "cook": cooked}
    commands = []

    def invoke(*args):
        commands.append(tuple(map(str, args)))
        raw = products[args[1]]
        digest = hashlib.sha256(raw).hexdigest()
        host(Path(args[4]) / (digest + ".rasset")).write_bytes(raw)
        return {"leaf": digest + ".rasset", "revision": digest}

    args = SimpleNamespace(tool="relative watch.exe", in_process="relative mesh.exe")
    namespace.update(root=base, a=args, identity="inert-identity", invoke=invoke)
    result = namespace["direct"](b"inert source", "direct")
    direct = base / "direct"
    imported_leaf = hashlib.sha256(imported).hexdigest() + ".rasset"
    check("actual direct helper reads its long-path source and products",
          result == (imported, cooked, hashlib.sha256(cooked).hexdigest())
          and host(direct / "source.obj").read_bytes() == b"inert source")
    check("direct import/cook argv retain exact ordinary roots and relative executable",
          commands == [(args.in_process, "import", str(direct), "source.obj", str(direct), "inert-identity"),
                       (args.in_process, "cook", str(direct), imported_leaf, str(direct))])
    command = one_node((n.value for n in ast.walk(tree) if isinstance(n, ast.Assign)
                        and any(isinstance(t, ast.Attribute) and t.attr == "command" for t in n.targets)),
                       "watch command expression", check)
    session = SimpleNamespace(path=base, source=base / "source", output=base / "output")
    argv = eval(compile(ast.Expression(command), str(SCRIPT), "eval"),
                dict(namespace, self=session, duration=3000, imports=16))
    check("watch argv retain ordinary roots and relative executable",
          argv == [args.tool, "--watch", str(session.source), "source.obj", str(session.output),
                   str(base / "stages"), "inert-identity", "3000", "16"])
    receipt["captured_arguments"] = {"direct": commands, "watch": argv, "tools_launched": False}


def binary_cases(out, tree, namespace, check, receipt):
    host = namespace["host_io_path"]
    expression = one_node((k.value for n in ast.walk(tree) if isinstance(n, ast.Call)
                           and isinstance(n.func, ast.Name) and n.func.id == "dict"
                           for k in n.keywords if k.arg == "binaries"), "binary receipt expression", check)
    code = compile(ast.Expression(expression), str(SCRIPT), "eval")
    first = long_path(out, "binary", "watch \u03bb.exe", 320, check)
    host(first.parent).mkdir(parents=True)
    paths = (first, first.parent / "mesh tool.exe")
    payloads = (b"inert watch", b"inert mesh")
    for path, raw in zip(paths, payloads):
        host(path).write_bytes(raw)
    relative = tuple(os.path.relpath(path, Path.cwd()) for path in paths)
    dotted = tuple(os.path.join(os.path.dirname(p), ".", os.path.basename(p)) for p in relative)
    parent = tuple(str(Path(p).parent / ".." / first.parent.name / Path(p).name) for p in relative)
    cases = (("relative", relative), ("dot", dotted), ("parent", parent),
             ("mixed", (str(first), relative[1])), ("absolute", tuple(map(str, paths))))
    for label, argv in cases:
        args = SimpleNamespace(tool=argv[0], in_process=argv[1])
        binaries = eval(code, dict(namespace, a=args, hashlib=hashlib))
        check("actual binary hashes match long-file bytes " + label,
              binaries == {p: hashlib.sha256(raw).hexdigest() for p, raw in zip(argv, payloads)})
        check("binary receipt retains exact argv keys " + label,
              tuple(binaries) == argv and (args.tool, args.in_process) == argv)
        receipt.setdefault("binary_paths", []).append({"case": label, "binaries": binaries})


def receipt_case(out, tree, namespace, check):
    host = namespace["host_io_path"]
    statement = one_node((n for n in ast.walk(tree) if isinstance(n, ast.Expr)
                          and isinstance(n.value, ast.Call) and isinstance(n.value.func, ast.Attribute)
                          and n.value.func.attr == "write_text"), "receipt write", check)
    path = long_path(out, "receipt", "receipt.json", 320, check)
    host(path.parent).mkdir(parents=True)
    payload = {"root": str(path.parent), "commands": [], "native_started": False}
    exec(compile(ast.Module(body=[statement], type_ignores=[]), str(SCRIPT), "exec"),
         dict(namespace, root=path.parent, receipt=payload, json=json))
    check("actual receipt write handles long path and preserves ordinary paths",
          json.loads(host(path).read_text(encoding="utf-8")) == payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    out = evidence_directory("quality-step0-mesh-watch-paths", args.output)
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
            check("Windows host path fixture", os.name == "nt")
            check("Python assertions enabled", sys.flags.optimize == 0 and not os.environ.get("PYTHONOPTIMIZE"))
            tree, namespace = load_fixture(check)
            path_cases(out, namespace, check)
            product_cases(out, tree, namespace, check, receipt)
            argument_cases(out, tree, namespace, check, receipt)
            binary_cases(out, tree, namespace, check, receipt)
            receipt_case(out, tree, namespace, check)
            receipt["status"] = "passed"
    except BaseException as error:
        receipt.update(status="failed", error=repr(error))
    finally:
        receipt["source_unchanged"] = (file_sha256(SCRIPT) == receipt["script_sha256"]
                                       and file_sha256(Path(__file__)) == receipt["tests_sha256"])
        if not receipt["source_unchanged"]:
            receipt.update(status="failed", error="Fixture source changed during checks")
        with (out / "tests.json").open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2, sort_keys=True)
    print(json.dumps({"status": receipt["status"], "checks": len(receipt["checks"]),
                      "error": receipt.get("error"), "receipt": str(out / "tests.json")}))
    return 0 if receipt["status"] == "passed" else 75 if receipt["status"] == "held" else 1


if __name__ == "__main__":
    raise SystemExit(main())
