"""Derive the approved pool-bounded EPA traversal and horizon corrections."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
from physics_build_overlay import ROOT, checked_output, write_immutable
from physics_dependencies import regular, relative_name

LOCK = Path(__file__).with_name('physics_epa_overlay.lock.json')
SOURCE = 'Jolt/Geometry/EPAConvexHullBuilder.h'
PATCH = 'race-jolt-5.6.0-epa-capacity-256-v2'
REPLACEMENTS = (
    (b'StackEntry stack[cMaxEdgeLength];', b'StackEntry stack[cMaxTriangles];'),
    (b'JPH_ASSERT(cur_stack_pos < cMaxEdgeLength);', b'JPH_ASSERT(cur_stack_pos < cMaxTriangles);'),
    (b'static constexpr int cMaxEdgeLength = 128;', b'static constexpr int cMaxEdgeLength = cMaxTriangles;'),
)


def derive(source: bytes, lock: dict) -> bytes:
    if (lock['schema'] != 1 or lock['patch'] != PATCH or lock['source'] != SOURCE
            or lock['maximum_triangles'] != 256 or lock['maximum_edges'] != 256 or lock['maximum_points'] != 128
            or lock['upstream_commit'] != 'e77f175595e64cb44218cc9d9d56fc365ad0e36a'):
        raise ValueError('Unsupported EPA build overlay')
    if hashlib.sha256(source).hexdigest() != lock['upstream_sha256']:
        raise ValueError('EPA upstream content differs')
    result = source
    for old, new in REPLACEMENTS:
        if result.count(old) != 1:
            raise ValueError('EPA anchor count differs')
        result = result.replace(old, new)
    if hashlib.sha256(result).hexdigest() != lock['derived_sha256']:
        raise ValueError('EPA derived content differs')
    return result


def generate(source: Path, output: Path, root: Path = ROOT) -> dict:
    output = checked_output(output, root, 'EPAConvexHullBuilder.h')
    if output.parts[-3:] != ('Jolt', 'Geometry', 'EPAConvexHullBuilder.h'):
        raise ValueError('EPA header must preserve its Jolt include path')
    lock = json.loads(LOCK.read_text(encoding='utf-8'))
    original = source / relative_name(lock['source'])
    regular(original)
    write_immutable(output, derive(original.read_bytes(), lock))
    return dict(schema=1, patch=PATCH, upstream=str(original), derived=str(output),
                include_root=str(output.parents[2]), upstream_sha256=lock['upstream_sha256'],
                derived_sha256=lock['derived_sha256'], maximum_triangles=256, maximum_edges=256, maximum_points=128,
                qualification_only=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(generate(args.source, args.output), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
