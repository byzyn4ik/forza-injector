"""Game-neutral host acceptance and provider lifecycle regressions."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def run_host_tests(output, zig, game_folder, result):
    build = output / 'tests' / 'host-probes'
    build.mkdir(parents=True, exist_ok=True)
    probes = {}
    for name, defines in {
        'first': ['EARLY'], 'second': ['EARLY'], 'legacy': ['LEGACY_ONLY'], 'early-fail': ['EARLY', 'EARLY_FAIL'],
        'decline': ['PROVIDER_RESULT=0'], 'claim': ['PROVIDER_RESULT=1'], 'fail': ['PROVIDER_RESULT=-1'],
    }.items():
        dll = build / (name + '.dll')
        command = [str(zig), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O2', '-Wall', '-Wextra',
            '-Werror', '-Wno-unused-variable', '-I', str(ROOT / 'sdk/include'), '-shared', '-s',
            str(ROOT / 'tests/host_probe.c'), '-DPROBE_NAME="' + name + '"', *['-D' + d for d in defines],
            '-o', str(dll)]
        subprocess.run(command, check=True, capture_output=True)
        probes[name] = dll
    runs = output / 'test-runs'
    runs.mkdir(parents=True, exist_ok=True)
    def scenario(name, mods=(), order=None, title='DEADBEEF', game='-', expect=True, provider=0,
                 includes=(), excludes=(), expected_order=()):
        with tempfile.TemporaryDirectory(prefix='host-' + name + '-', dir=runs) as temporary:
            directory = Path(temporary)
            for folder, probe, extra in mods:
                plugin = directory / 'mods' / folder / 'plugins'
                plugin.mkdir(parents=True)
                shutil.copy2(probes[probe], plugin / (probe + '.dll'))
                manifest = {'format_version': 1, 'id': folder, 'name': folder, 'games': [], 'dependencies': []}
                manifest.update(extra)
                (plugin.parent / 'mod.json').write_text(json.dumps(manifest), encoding='utf-8')
            if order is not None:
                (directory / 'mods').mkdir(exist_ok=True)
                (directory / 'mods/loadorder.txt').write_bytes(order.encode('utf-8'))
            command = [sys.executable, '-I', str(ROOT / 'tests/host_case.py'),
                str(output / 'core-test/winmm.dll'), str(directory), title, str(game)]
            process = subprocess.run(command, capture_output=True, text=True, timeout=30)
            try:
                report = json.loads(process.stdout.splitlines()[-1]) if not process.returncode else None
            except (ValueError, IndexError):
                report = None
            if report is None:
                result('FAIL', f'host {name}: child failed ({process.returncode}): {process.stderr[-400:]}')
                return
            log = report['log']
            ok = bool(report['ok']) == expect and report['waited'] and report['unchanged'] and report['provider'] == provider and report['package_agnostic']
            ok = ok and all(item in log for item in includes) and all(item not in log for item in excludes)
            if expected_order:
                indexes = [log.find(item) for item in expected_order]
                ok = ok and all(i >= 0 for i in indexes) and indexes == sorted(indexes)
            result('ok' if ok else 'FAIL', f'host {name}: {report if not ok else "expected lifecycle; game image unchanged"}')
    scenario('unknown-game-no-mods')
    scenario('legacy-v1', [('Legacy', 'legacy', {})], includes=['legacy:v1'])
    scenario('dual-abi', [('First', 'first', {})], includes=['first:early', 'first:v2', 'first:worker', '|DEADBEEF'],
             excludes=['first:v1', 'initialization refused'])
    scenario('early-failure', [('First', 'early-fail', {})], includes=['early-fail:early'], excludes=['early-fail:v2'])
    scenario('early-before-normal', [('First', 'first', {}), ('Second', 'second', {})],
             expected_order=['first:early', 'second:early', 'first:v2', 'second:v2'])
    scenario('loadorder', [('First', 'first', {}), ('Second', 'second', {})], order='Second\nFirst\n',
             expected_order=['second:early', 'first:early', 'second:v2', 'first:v2'])
    scenario('disabled-folder', [('First', 'first', {}), ('Second', 'second', {})],
             order='# First\nSecond\n', includes=['second:v2'], excludes=['first:'])
    scenario('disabled-manifest', [('First', 'first', {'enabled': False})], excludes=['first:'])
    scenario('enabled-manifest', [('First', 'first', {'enabled': True})], includes=['first:v2'])
    scenario('game-filter', [('First', 'first', {'games': ['fh2']})], excludes=['first:'])
    scenario('title-filter', [('First', 'first', {'games': ['deadbeef']})], includes=['first:v2'])
    scenario('dependency-order', [('First', 'first', {}), ('Second', 'second', {'dependencies': ['First']})],
             order='Second\nFirst\n', includes=['first:v2'], excludes=['second:'])
    scenario('duplicate-id', [('First', 'first', {'id': 'same'}), ('Second', 'second', {'id': 'same'})],
             includes=['first:v2'], excludes=['second:'])
    scenario('duplicate-loadorder', [('First', 'first', {})], order='First\nFirst\n', expect=False, excludes=['first:'])
    scenario('traversal-loadorder', [('First', 'first', {})], order='../First\n', expect=False, excludes=['first:'])
    scenario('provider-declines', [('AAA', 'first', {}), ('ZZZ', 'decline', {})],
             expected_order=['decline:provider', 'first:early', 'first:v2'])
    scenario('provider-claims', [('AAA', 'first', {}), ('ZZZ', 'claim', {})], provider=1,
             includes=['claim:provider'], excludes=['first:', 'claim:v2'])
    scenario('provider-fails', [('AAA', 'first', {}), ('ZZZ', 'fail', {})], expect=False, provider=-1,
             includes=['fail:provider'], excludes=['first:', 'fail:v2'])
    paths = [('fh2', '1A96545B', game_folder / 'Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/forza_durango_release_final.exe'),
             ('fh2ff', '5A9771A4', game_folder / 'Forza Horizon 2 Presents Fast & Furious/Spire_1.0.0.6_x64__8wekyb3d8bbwe/Mount/forza_durango_release_final.exe')]
    for game_id, title, exe in paths:
        if exe.is_file():
            scenario('real-' + game_id, [('First', 'first', {'games': [game_id]})], title=title, game=exe,
                     includes=['first:v2', game_id + '|' + title])
        else:
            result('skip', f'host real-{game_id}: game EXE not present')
