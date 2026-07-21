#!/usr/bin/env python3
"""Focused checks for exact-hash BIOS-resident shard publication/loading."""
import importlib.util
import json
import pathlib
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    'compile_overlays', ROOT / 'tools' / 'compile_overlays.py')
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MOD)


def main():
    recipe = json.loads((ROOT / 'tools' / 'aot_overlay_spike' /
                         'bios_resident_code.json').read_text(encoding='utf-8'))
    image = recipe['images'][0]
    region_start = int(image['region_start'], 0)
    region_size = int(image['region_size'], 0)
    words = image['code_fragments'][0]['words']
    # Never regress to a page envelope: the rejected prototype exposed its
    # trailing zeros as a seventh function (data-as-code).
    assert region_start == 0x8000DF80
    assert region_size == len(words) * 4 == 0x70
    assert not image.get('data_words')
    assert len(image['dispatch_entry_pcs']) == 6

    with tempfile.TemporaryDirectory() as td:
        dll = pathlib.Path(td) / '0000DF80_4EE6AC69.dll'
        marker = dll.with_suffix('.resident')
        cap = {
            'producer': MOD.BIOS_RESIDENT_PRODUCER,
            'bios_sha256': 'ab' * 32,
            'producer_name': 'test resident helper',
        }
        MOD.update_bios_resident_marker(str(dll), cap)
        payload = json.loads(marker.read_text(encoding='utf-8'))
        assert payload['schema'] == MOD.BIOS_RESIDENT_MARKER
        assert payload['bios_sha256'] == 'ab' * 32

        # Reusing the output stem for an ordinary capture must not leave an
        # eager-preload marker behind.
        MOD.update_bios_resident_marker(str(dll), {})
        assert not marker.exists()

    source = (ROOT / 'runtime' / 'src' / 'overlay_loader.c').read_text(
        encoding='utf-8')
    assert 'BIOS_RESIDENT_MARKER_SCHEMA' in source
    assert source.count('load_bios_resident_shards();') == 2
    assert 'cache_path_is_bios_resident' in source

    print('ALL PASS')


if __name__ == '__main__':
    main()
