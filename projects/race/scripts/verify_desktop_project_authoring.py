"""Verify actual Qt project/recipe authoring, then the shared import and Qt/SDL workflow.

The first-party CLI creates an independent reference closure only. The editor
must create the initially absent project and recipes through its native controls.
Use --build, --output and optional --tools exactly as in the checkpoint harness.
"""
from test_content_project import binary_index, parse_binary_index
import hashlib
import json
import math
from pathlib import Path
import runpy
import sys


def digest(blob):
    return hashlib.sha256(blob).hexdigest()


def prepare_reference(root, identity, run, check, label):
    """Create deterministic CLI oracle files without populating the editor root."""
    reference = root / 'reference-recipes'
    reference.mkdir(exist_ok=False)
    recipe_leaves = []
    expected = {}
    for seed, asset, source in [('main-seed.json', identity, 'b.obj'),
                                ('other-seed.json', '000000000000004d0000000000000002', 'unused.obj')]:
        result = json.loads(run('race_mesh_import_tool.exe', '--create-recipe', reference, seed, asset, source))
        raw = (reference / seed).read_bytes()
        revision = digest(raw)
        check(label + ' reference recipe ' + source, result['recipe_revision'] == revision and
              result['asset'] == asset and result['source'] == source)
        leaf = revision + '.json'
        (reference / leaf).write_bytes(raw)
        expected[leaf] = raw
        recipe_leaves.append(leaf)
    for leaf, recipes in [('empty.raceproject', []), ('one.raceproject', recipe_leaves[:1]),
                          ('project.raceproject', recipe_leaves)]:
        references = [dict(asset=json.loads(expected[name])['asset'], kind='mesh',
                           recipe=name, revision=digest(expected[name])) for name in recipes]
        arguments = [value for item in references for value in
                     ('mesh', item['asset'], item['recipe'], item['revision'])]
        result = json.loads(run('race_content_tool.exe', 'create', reference, leaf, *arguments))
        raw = (reference / leaf).read_bytes()
        check(label + ' reference binary index ' + leaf, raw == binary_index(references) and
              result['encoding'] == 'binary' and result['wire_version'] == 1 and
              result['revision'] == digest(raw) and result['entries'] == len(recipes))
        expected[leaf] = raw
    check(label + ' authoring destination initially empty', not any((root / 'recipes').iterdir()))
    return dict(root=reference, main_recipe=recipe_leaves[0], expected=expected)


def verify_authoring(root, reference, receipt, check, label):
    """Compare exact authored bytes and preserved closures with the CLI oracle."""
    recipes = root / 'recipes'
    records = []
    for leaf, raw in reference['expected'].items():
        path = recipes / leaf
        check(label + ' authored exact bytes ' + leaf, path.is_file() and path.read_bytes() == raw)
        records.append(dict(path=path.relative_to(root).as_posix(), bytes=len(raw), sha256=digest(raw),
                            reference=(reference['root'] / leaf).relative_to(root).as_posix()))
    for leaf, count in [('empty.raceproject', 0), ('one.raceproject', 1), ('project.raceproject', 2)]:
        index = parse_binary_index((recipes / leaf).read_bytes())
        check(label + ' preserved index entries ' + leaf, index['schema'] == 2 and len(index['recipes']) == count)
        for entry in index['recipes']:
            recipe_leaf = entry['recipe']
            check(label + ' digest recipe name ' + leaf + ' ' + recipe_leaf,
                  len(recipe_leaf) == 69 and recipe_leaf == entry['revision'] + '.json' and
                  all(c in '0123456789abcdef' for c in entry['revision']))
            raw = (recipes / recipe_leaf).read_bytes()
            recipe = json.loads(raw)
            check(label + ' exact raw closure ' + leaf + ' ' + recipe_leaf,
                  digest(raw) == entry['revision'] and recipe['asset'] == entry['asset'] and
                  raw == reference['expected'][recipe_leaf])
            check(label + ' source remains present ' + recipe_leaf, (root / 'source' / recipe['source']).is_file())
    authoring_checks = receipt.get('authoring_checks', [])
    check(label + ' native authoring checks', bool(authoring_checks) and
          all(c.get('passed') is True for c in authoring_checks))
    measurements = receipt.get('authoring_measurements', [])
    successful = [m for m in measurements if m.get('passed') is True]
    check(label + ' create add add publications', len(successful) == 3 and
          [m['entries'] for m in successful] == [0, 1, 2])
    timing_keys = ['admission_ms', 'revalidate_ms', 'source_ms', 'publish_ms',
                   'readback_ms', 'publication_ms', 'qt_publication_ms']
    check(label + ' bounded authoring measurements', all(
        all(isinstance(m[k], (int, float)) and math.isfinite(m[k]) and m[k] >= 0 for k in timing_keys) and
        0 <= m['source_bytes'] <= 8 * 1024 * 1024 and 0 <= m['retained_bytes'] <= 256 * 1024 and
        0 <= m['entries'] <= 32 for m in measurements))
    check(label + ' exact authored source read sizes',
          [m['source_bytes'] for m in successful] ==
          [0, (root / 'source/b.obj').stat().st_size, (root / 'source/unused.obj').stat().st_size])
    return dict(name=label, files=records, native_checks=len(authoring_checks),
                successful_publications=len(successful), measurements=measurements)


if __name__ == '__main__':
    harness = Path(__file__).with_name('verify_desktop_mesh_checkpoint.py')
    sys.argv = [str(harness), *sys.argv[1:]]
    if '--authoring' not in sys.argv:
        sys.argv.append('--authoring')
    runpy.run_path(str(harness), run_name='__main__')
