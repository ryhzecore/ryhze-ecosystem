"""Re-encode the licensed original WOFF files without subsetting or table transforms.

Optional regeneration tool: Python 3, fontTools[woff] 4.61.1, Brotli 1.2.0.
The resulting WOFF2 files are checked in; normal builds need only Node/npm.
"""
from pathlib import Path
import hashlib
import json
from fontTools.ttLib import TTFont
from fontTools.ttLib.woff2 import compress

ROOT = Path(__file__).resolve().parents[1] / 'public' / 'fonts'
records = []
for family in ('Inter', 'SpaceGrotesk'):
    source, target = ROOT / f'{family}.woff', ROOT / f'{family}.woff2'
    compress(source, target, transform_tables=set())
    original, encoded = TTFont(source), TTFont(target)
    removed = set(original.reader.keys()) - set(encoded.reader.keys())
    assert removed <= {'DSIG'}
    assert not (set(encoded.reader.keys()) - set(original.reader.keys()))
    if removed:
        # WOFF2 omits the original's empty, non-rendering signature placeholder.
        assert original.reader['DSIG'] == bytes.fromhex('0000000100000000')
    for tag in encoded.reader.keys():
        before, after = original.reader[tag], encoded.reader[tag]
        if tag == 'head':
            # WOFF2 rewrites the checksum and sets the lossless-compression flag.
            assert int.from_bytes(after[16:18], 'big') == (
                int.from_bytes(before[16:18], 'big') | (1 << 11))
            before = before[:8] + before[12:16] + before[18:]
            after = after[:8] + after[12:16] + after[18:]
        assert before == after, f'{family}: changed {tag} table'
    assert original.getBestCmap() == encoded.getBestCmap()
    assert original.getGlyphOrder() == encoded.getGlyphOrder()
    records.append({
        'family': family,
        'sourceSha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'woff2Sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
        'sourceBytes': source.stat().st_size,
        'woff2Bytes': target.stat().st_size,
        'glyphs': len(original.getGlyphOrder()),
        'unicodeMappings': len(original.getBestCmap()),
        'axes': [{'tag': a.axisTag, 'min': a.minValue, 'default': a.defaultValue, 'max': a.maxValue} for a in original['fvar'].axes],
        'byteIdenticalTables': sorted(set(encoded.reader.keys()) - {'head'}),
        'allowedContainerChanges': ['head checksum adjustment', 'head bit 11 lossless-compression flag'] + (['empty DSIG placeholder omitted'] if removed else []),
    })
    print(f'{family}: {source.stat().st_size} -> {target.stat().st_size} bytes; '
          f'{len(original.getGlyphOrder())} glyphs, all rendering tables verified')

(ROOT.parents[1] / 'docs' / 'font-encoding.json').write_text(json.dumps(records, indent=2) + '\n')
