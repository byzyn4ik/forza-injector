"""Optional paired-repository test: generic host + the real Extra Options provider."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def run_provider_integration(output, game_folder, result):
    # The plugins repository beside this one (FH2-ExtraOptions-Injector-Plugins, cloned as either name).
    plugin_repo = next((ROOT.parent / name for name in ('forza-plugins', 'FH2-ExtraOptions-Injector-Plugins')
                        if (ROOT.parent / name).is_dir()), ROOT.parent / 'forza-plugins')
    mod = plugin_repo / 'out/build/xtra-options/mods/Xtra Options'
    patches = plugin_repo / 'plugins/xtra-options/runtime/src/patches.json'
    exe = game_folder / 'Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/forza_durango_release_final.exe'
    if not (mod / 'plugins/XtraRuntime.dll').is_file() or not patches.is_file() or not exe.is_file():
        result('skip', 'paired runtime: build sibling forza-plugins Xtra Options and supply FH2 EXE')
        return
    with tempfile.TemporaryDirectory(prefix='paired-runtime-', dir=output / 'test-runs') as temporary:
        directory = Path(temporary)
        shutil.copytree(mod, directory / 'mods/Xtra Options')
        process = subprocess.run([sys.executable, '-I', str(ROOT / 'tests/host_case.py'),
            str(output / 'core-test/winmm.dll'), str(directory), '1A96545B', str(exe), str(patches)],
            capture_output=True, text=True, timeout=120)
        try:
            report = json.loads(process.stdout.splitlines()[-1]) if not process.returncode else None
        except (ValueError, IndexError):
            report = None
        if report is None:
            result('FAIL', f'paired runtime: child failed ({process.returncode}): {process.stderr[-600:]}')
            return
        result('ok' if report['ok'] and report['provider'] == 1 else 'FAIL',
               'paired runtime: the real XtraRuntime.dll claims FH2 through the lean host')
        data = json.loads(patches.read_text(encoding='utf-8'))['patches']
        garage = [p for p in data if p['runtime_group'] == 'PAUSE_GARAGE']
        intro = [p for p in data if p['runtime_group'] == 'INTRO']
        result('ok' if garage and all(report['sites'][p['id']] == p['patched'] for p in garage) else 'FAIL',
               'paired runtime: Extra Options garage behavior moved into provider')
        result('ok' if intro and all(report['sites'][p['id']] == p['patched'] for p in intro) else 'FAIL',
               'paired runtime: individual SkipIntros DLL is initialized by provider early lifecycle')
        result('ok' if 'game runtime claimed' in report['log'] and 'initialization complete.' not in report['log'] else 'FAIL',
               'paired runtime: generic host does not duplicate the provider plugin lifecycle')
