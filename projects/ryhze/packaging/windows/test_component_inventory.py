import json
from pathlib import Path
import tempfile
import unittest
from component_inventory import make_inventory


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ('a', 'dev'):
            (self.root / name).mkdir()
            (self.root / name / 'LICENSE').write_text('Fixture notice only')
        self.config = self.root / 'config.json'
        self.config.write_text(json.dumps({'packages': [{'name': n, 'rootUri': n} for n in ('a', 'dev')]}))
        self.deps = self.root / 'deps.json'
        self.deps.write_text(json.dumps({'root': 'app', 'packages': [
            {'name': 'app', 'version': '0.6.0+3', 'directDependencies': ['a']},
            *[{'name': n, 'version': '1', 'source': 'hosted', 'dependencies': []} for n in ('a', 'dev')]]}))
        self.lock = self.root / 'lock'
        self.lock.write_text('packages:\n' + ''.join(f'  {n}:\n    source: hosted\n    version: "1"\n      sha256: {"a"*64}\n' for n in ('a', 'dev')))
        self.sdk = self.root / 'sdk.json'
        self.sdk.write_text(json.dumps({'frameworkRevision': 'fixture', 'engineRevision': 'fixture', 'dartSdkVersion': 'fixture'}))
        self.fonts = self.root / 'fonts'; self.fonts.mkdir()
        (self.fonts / 'Fixture-LICENSE.txt').write_text('Fixture font notice')
        self.notices = self.root / 'NOTICES.Z'; self.notices.write_bytes(b'fixture')

    def build(self):
        return make_inventory(self.config, self.deps, self.lock, self.sdk, self.fonts, self.notices)

    def test_deterministic_inventory_separates_runtime_and_development(self):
        first = self.build(); self.assertEqual(first, self.build())
        self.assertEqual([x['scope'] for x in first[0]['components']], ['runtime', 'development'])
        self.assertNotIn(str(self.root), json.dumps(first))

    def test_changed_dependency_or_missing_notice_fails_closed(self):
        self.lock.write_text(self.lock.read_text().replace('version: "1"', 'version: "2"'))
        with self.assertRaises(ValueError): self.build()

    def test_missing_notice_is_not_silently_omitted(self):
        (self.root / 'a' / 'LICENSE').unlink()
        with self.assertRaises(ValueError): self.build()

    def test_remote_package_root_is_never_fetched(self):
        self.config.write_text(json.dumps({'packages': [{'name': 'a', 'rootUri': 'https://example.invalid/a'}, {'name': 'dev', 'rootUri': 'dev'}]}))
        with self.assertRaises(ValueError): self.build()
