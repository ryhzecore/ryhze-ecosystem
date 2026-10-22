"""Inert checks for scripts/cpp_function_sizes.py. Reads only in-memory fixtures."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cpp_function_sizes as sizes


def lengths(text):
    return [(f["name"], f["length"]) for f in sizes.functions(text.split("\n"))]


def main():
    checks = []

    def check(name, value):
        checks.append(name)
        if not value:
            raise AssertionError(name)

    check("free function measured to its closing brace",
          lengths("int add(int a, int b) {\n  return a + b;\n}") == [("add", 3)])
    check("member defined out of line keeps its qualified name",
          lengths("void Editor::frame_tick() {\n  if (x) {\n    y();\n  }\n}") == [("Editor::frame_tick", 5)])
    check("namespace and class openings are not functions",
          lengths("namespace race {\nclass A {\n  void f() {\n  }\n};\n}") == [("f", 2)])
    check("braces inside strings and comments are ignored",
          lengths('void f() {\n  auto s = "{{{";  // }}}\n  char c = \'{\';\n}') == [("f", 4)])
    check("control statements inside a function are not separate functions",
          lengths("void f() {\n  for (int i = 0; i < 3; ++i) {\n    g(i);\n  }\n}") == [("f", 5)])
    check("lambda bodies count toward their enclosing function",
          lengths("void f() {\n  auto h = [] {\n    return 1;\n  };\n}") == [("f", 5)])
    check("multi-line constructor is measured from its signature, not its initializer list",
          lengths("Editor::Editor(int a,\n               int b)\n    : a_(a), b_(b) {\n  run();\n}")
          == [("Editor::Editor", 5)])
    long_body = "void big() {\n" + "  step();\n" * 85 + "}"
    result = sizes.report([], Path.cwd())
    check("empty scan reports nothing over limit", not result["files_over_limit"] and not result["functions_over_limit"])
    check("an 87-line function exceeds the 80-line limit", lengths(long_body)[0][1] == 87 > sizes.FUNCTION_LIMIT)
    print(f"cpp_function_sizes: {len(checks)} checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
