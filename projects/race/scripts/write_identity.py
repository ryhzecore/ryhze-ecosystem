"""Refresh an embedded identity on every build; write only when it changes."""
import argparse
import json
from pathlib import Path
from source_manifest import authored_inputs, git_state
parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--configuration", required=True)
parser.add_argument("--sanitizer", required=True)
args = parser.parse_args()
values = {"source_commit": git_state()["commit"],
          "authored_input_sha256": authored_inputs()["sha256"],
          "configuration": args.configuration, "sanitizer": args.sanitizer}
compiler = """#if defined(__clang__)
inline constexpr const char* compiler_version = __clang_version__;
#elif defined(_MSC_VER)
#define RACE_STRINGIFY_IMPL(x) #x
#define RACE_STRINGIFY(x) RACE_STRINGIFY_IMPL(x)
inline constexpr const char* compiler_version = "MSVC " RACE_STRINGIFY(_MSC_FULL_VER);
#else
inline constexpr const char* compiler_version = "unknown";
#endif
"""
text = "#pragma once\nnamespace race::build {\n" + compiler + "".join(
    f"inline constexpr const char* {key} = {json.dumps(value)};\n" for key, value in values.items()) + "}\n"
args.output.parent.mkdir(parents=True, exist_ok=True)
if not args.output.exists() or args.output.read_text() != text:
    args.output.write_text(text)
