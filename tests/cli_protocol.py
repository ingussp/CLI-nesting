"""Exercise CLI input, export, cancellation and result publication."""
import argparse
import json
import os
import signal
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import uuid
import xml.etree.ElementTree as ET


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nesting protocol ')
        self.root = Path(self.temp.name)
        self.process = None

    def tearDown(self):
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        self.temp.cleanup()

    def job(self, mode):
        token = str(uuid.uuid4())
        data = dict(job_id=token, created_at='2026-10-11T10:00:00Z', units='mm',
                    config=dict(mode=mode, threads=2, continuousRoundSeconds=.1,
                                timeLimitSeconds=0),
                    sheets=[dict(points=[[0, 0], [100, 0], [100, 100], [0, 100]], quantity=1)],
                    parts=[dict(id='part_0', points=[[0, 0], [10, 0], [10, 10], [0, 10]],
                                quantity=12, rotations=1)],
                    output=dict(json='result.json', cancelFile='.cancel-' + token))
        path = self.root / 'input.json'
        path.write_text(json.dumps(data), encoding='utf-8')
        return path, data

    def test_first_result_geometry(self):
        path, data = self.job('first')
        completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                   capture_output=True, text=True, timeout=15)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads((self.root / 'result.json').read_text(encoding='utf-8'))
        self.assertEqual(result['schemaVersion'], 1)
        self.assertEqual(result['units'], 'mm')
        self.assertEqual(result['job_id'], data['job_id'])
        self.assertEqual(result['created_at'], data['created_at'])
        self.assertEqual({p['source'] for s in result['sheets'] for p in s['parts']}, {'part_0'})
        self.assertEqual(result['placed'], 12)
        self.assertFalse((self.root / 'result.json.tmp').exists())


    def test_continuous_history_latest_and_cooperative_cancel(self):
        history = self.root / 'results'
        history.mkdir()
        (history / 'obsolete.json').write_text('old job')
        path, data = self.job('continuous')
        self.process = subprocess.Popen([EXE, '--input', str(path), '--dxf', 'result.dxf', '--svg', 'result.svg'], cwd=self.root,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 10
        result = None
        while time.monotonic() < deadline:
            try:
                result = json.loads((self.root / 'result.json').read_text(encoding='utf-8'))
                break
            except (OSError, ValueError):
                self.assertIsNone(self.process.poll())
                time.sleep(.02)
        self.assertIsNotNone(result, 'No latest continuous snapshot')
        self.assertEqual(result['placed'], 12)
        self.assertEqual(result['job_id'], data['job_id'])
        self.assertEqual(result['created_at'], data['created_at'])
        self.assertFalse((history / 'obsolete.json').exists())
        self.assertIsNone(self.process.poll(), 'Continuous search must stay running')
        (self.root / data['output']['cancelFile']).touch()
        _, errors = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 0, errors)
        snapshots = sorted(history.glob('result*.json'), key=lambda p: int(p.stem[6:]))
        self.assertTrue(snapshots)
        for snapshot in snapshots:
            self.assertTrue(snapshot.with_suffix('.dxf').is_file())
            ET.parse(snapshot.with_suffix('.svg'))
        latest = json.loads((self.root / 'result.json').read_text(encoding='utf-8'))
        previous = json.loads(snapshots[-1].read_text(encoding='utf-8'))
        self.assertEqual(latest['sheets'], previous['sheets'])
        self.assertEqual(latest['unplaced'], previous['unplaced'])
        self.assertEqual(latest['job_id'], data['job_id'])
        self.assertEqual(latest['created_at'], data['created_at'])
        self.assertFalse(latest['timeLimitReached'])
        self.assertFalse(list(self.root.rglob('*.tmp')))
        self.assertEqual(latest['stopReason'], 'user_stop')
        log = (self.root / 'recursive-progress.log').read_text()
        self.assertIn('New job:', log)
        self.assertIn('Recursive finished:', log)
        self.assertIn('cancelled=1', log)

    @unittest.skipUnless(os.name == 'nt', 'Windows file sharing semantics')
    def test_latest_publication_retries_while_reader_holds_file(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        path, data = self.job('continuous')
        self.process = subprocess.Popen([EXE, '--input', str(path)], cwd=self.root,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        latest = self.root / 'result.json'
        deadline = time.monotonic() + 10
        while not latest.exists() and time.monotonic() < deadline:
            self.assertIsNone(self.process.poll())
            time.sleep(.01)
        self.assertTrue(latest.exists())
        # Permit reading/writing, deliberately deny FILE_SHARE_DELETE.
        handle = kernel.CreateFileW(str(latest), 0x80000000, 3, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            (self.root / data['output']['cancelFile']).touch()
            time.sleep(.4)
            self.assertIsNone(self.process.poll(), 'Writer must wait for the transient reader')
        finally:
            kernel.CloseHandle(handle)
        _, errors = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 0, errors)
        result = json.loads(latest.read_text())
        self.assertEqual(result['placed'], 12)
        self.assertEqual(result['stopReason'], 'user_stop')
        self.assertFalse(list(self.root.rglob('*.tmp')))

    def test_continuous_deadline_stops_seed_and_parallel_workers(self):
        for threads, copies, seed_budget in ((1, 1, .01), (4, 16, .01), (2, 12, 30)):
            with self.subTest(threads=threads, copies=copies, seed_budget=seed_budget):
                path, data = self.job('continuous')
                data['config'].update(threads=threads, continuousRoundSeconds=seed_budget,
                                      timeLimitSeconds=.2, gpu=False)
                data['parts'][0]['quantity'] = copies
                data['output'].update(dxf=True, svg=True)
                path.write_text(json.dumps(data), encoding='utf-8')
                started = time.monotonic()
                completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                           capture_output=True, text=True, timeout=5)
                elapsed = time.monotonic() - started
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertGreaterEqual(elapsed, .18)
                self.assertLess(elapsed, 4)
                latest = json.loads((self.root / 'result.json').read_text())
                self.assertEqual(latest['mode'], 'continuous')
                self.assertEqual(latest['stopReason'], 'time_limit')
                self.assertTrue(latest['timeLimitReached'])
                self.assertEqual(latest['timeLimitSeconds'], .2)
                self.assertEqual(latest['job_id'], data['job_id'])
                self.assertEqual(latest['created_at'], data['created_at'])
                self.assertEqual(latest['placed'], copies)
                snapshots = sorted((self.root / 'results').glob('result*.json'),
                                   key=lambda p: int(p.stem[6:]))
                self.assertTrue(snapshots)
                self.assertEqual([p.stem for p in snapshots],
                                 ['result' + str(i+1) for i in range(len(snapshots))])
                qualities = []
                for snapshot in snapshots:
                    previous = json.loads(snapshot.read_text())
                    self.assertEqual(previous['timeLimitSeconds'], .2)
                    self.assertTrue(snapshot.with_suffix('.dxf').is_file())
                    ET.parse(snapshot.with_suffix('.svg'))
                    qualities.append((previous['unplacedCount'], previous['usedSheetWasteArea'],
                                      previous['compactWasteArea'], previous['placementSpreadCost']))
                for earlier, current in zip(qualities, qualities[1:]):
                    self.assertLess(current, earlier, 'History must contain strict improvements only')
                self.assertEqual(latest['sheets'], previous['sheets'])
                self.assertEqual(latest['unplaced'], previous['unplaced'])
                self.assertFalse(list(self.root.rglob('*.tmp')))

    @unittest.skipIf(os.name == 'nt', 'SIGINT delivery uses the POSIX console handler')
    def test_ctrl_c_stops_before_deadline(self):
        path, data = self.job('continuous')
        data['config']['timeLimitSeconds'] = 30
        path.write_text(json.dumps(data))
        self.process = subprocess.Popen([EXE, '--input', str(path)], cwd=self.root,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 5
        while not (self.root / 'results' / 'result1.json').exists():
            self.assertIsNone(self.process.poll())
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)
        self.process.send_signal(signal.SIGINT)
        _, errors = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 0, errors)
        latest = json.loads((self.root / 'result.json').read_text())
        self.assertEqual(latest['stopReason'], 'user_stop')
        self.assertFalse(latest['timeLimitReached'])

    def test_only_supported_modes_and_deadlines_are_accepted(self):
        for config in ({'mode': 'unknown'}, {'mode': 'first', 'timeLimitSeconds': 1},
                       {'mode': 'continuous', 'timeLimitSeconds': -1},
                       {'mode': 'continuous', 'timeLimitSeconds': 86401}):
            with self.subTest(config=config):
                path, data = self.job('first')
                data['config'].update(config)
                path.write_text(json.dumps(data))
                history = self.root / 'results'
                history.mkdir(exist_ok=True)
                sentinel = history / 'keep.json'
                sentinel.write_text('previous job')
                completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                           capture_output=True, text=True, timeout=5)
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(sentinel.read_text(), 'previous job')
                self.assertFalse((self.root / 'result.json').exists())

    def test_positive_budget_without_mode_selects_continuous(self):
        path, data = self.job('first')
        del data['config']['mode']
        data['config']['timeLimitSeconds'] = .1
        path.write_text(json.dumps(data))
        completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                   capture_output=True, text=True, timeout=5)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        latest = json.loads((self.root / 'result.json').read_text())
        self.assertEqual(latest['mode'], 'continuous')
        self.assertEqual(latest['stopReason'], 'time_limit')

    def test_cancel_path_cannot_overwrite_input(self):
        path, data = self.job('first')
        data['output']['cancelFile'] = 'input.json'
        path.write_text(json.dumps(data), encoding='utf-8')
        completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                   capture_output=True, text=True, timeout=10)
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(json.loads(path.read_text(encoding='utf-8')), data)

    def test_unknown_fields_are_rejected(self):
        for location in ('root', 'part', 'sheet', 'hole', 'point', 'config', 'output'):
            with self.subTest(location=location):
                path, data = self.job('first')
                obj = {'root': data, 'part': data['parts'][0], 'sheet': data['sheets'][0],
                       'config': data['config'], 'output': data['output']}.get(location)
                if location == 'hole':
                    obj = dict(points=[[1, 1], [3, 1], [3, 3], [1, 3]])
                    data['parts'][0]['holes'] = [obj]
                elif location == 'point':
                    obj = dict(x=0, y=0)
                    data['parts'][0]['points'][0] = obj
                obj['extraNote'] = 'unused'
                path.write_text(json.dumps(data), encoding='utf-8')
                completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                           capture_output=True, text=True, timeout=10)
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn('extraNote', completed.stderr)
                self.assertFalse((self.root / 'result.json').exists())

    def test_search_options_must_affect_the_engine(self):
        for key in ('algorithm', 'rotationStep', 'rotations', 'trials'):
            with self.subTest(key=key):
                path, data = self.job('first')
                data['config'][key] = 4
                path.write_text(json.dumps(data), encoding='utf-8')
                completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                           capture_output=True, text=True, timeout=10)
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn(key, completed.stderr)

    def test_geometry_is_two_dimensional(self):
        path, data = self.job('first')
        data['parts'][0]['points'][0].append(42)
        path.write_text(json.dumps(data), encoding='utf-8')
        completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                   capture_output=True, text=True, timeout=10)
        self.assertNotEqual(completed.returncode, 0)

    def test_stock_orientation_cannot_be_ignored(self):
        path, data = self.job('first')
        data['sheets'][0]['rotation'] = 45
        path.write_text(json.dumps(data), encoding='utf-8')
        completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                   capture_output=True, text=True, timeout=10)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn('rotation', completed.stderr)

    def test_dxf_sheets_parts_holes_and_labels_move_together(self):
        path, data = self.job('first')
        data['config'].update(spacing=2, partToSheet=2)
        data['sheets'] = [
            dict(points=[[10, 20], [110, 20], [110, 100], [10, 100]],
                 holes=[[[102, 92], [106, 92], [106, 96], [102, 96]]]),
            dict(points=[[-40, -20], [80, -20], [80, 50], [-40, 50]],
                 holes=[[[70, 44], [74, 44], [74, 48], [70, 48]]]),
            dict(points=[[230, -70], [320, -70], [320, 10], [230, 10]])]
        data['parts'] = [dict(id='panel<&', quantity=3, allowedAngles=[90],
                              points=[[0, 0], [60, 0], [60, 60], [0, 60]],
                              holes=[[[20, 20], [30, 20], [30, 30], [20, 30]]])]
        path.write_text(json.dumps(data), encoding='utf-8')
        completed = subprocess.run([EXE, '--input', str(path), '--dxf', 'layout.dxf', '--svg', 'layout.svg'],
                                   cwd=self.root, capture_output=True, text=True, timeout=15)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads((self.root / 'result.json').read_text())
        self.assertEqual(result['placed'], 3)
        self.assertEqual(len(result['sheets']), 3)
        self.assertEqual(len(ET.parse(self.root / 'layout.svg').getroot().findall('{http://www.w3.org/2000/svg}g')), 3)
        lines = (self.root / 'layout.dxf').read_text().splitlines()
        entities, entity = [], None
        for code, value in zip(lines[::2], lines[1::2]):
            if int(code) == 0:
                if entity is not None: entities.append(entity)
                entity = {'type': value, 'points': []}
            elif entity is not None:
                if int(code) == 8: entity['layer'] = value
                elif int(code) == 10: entity['points'].append([float(value), None])
                elif int(code) == 20: entity['points'][-1][1] = float(value)
        if entity is not None: entities.append(entity)
        entities = [e for e in entities if e['type'] in ('LWPOLYLINE', 'TEXT')]
        sheets = [e['points'] for e in entities if e.get('layer') == 'SHEETS']
        self.assertEqual(len(sheets), 3)
        for left, right in zip(sheets, sheets[1:]):
            self.assertAlmostEqual(min(x for x, y in right) - max(x for x, y in left), 20)
            self.assertAlmostEqual(min(y for x, y in right), min(y for x, y in left))
        self.assertEqual(sheets[0], result['sheets'][0]['points'])
        cursor = 0
        def match(layer, points, offset):
            nonlocal cursor
            entity = entities[cursor]; cursor += 1
            self.assertEqual(entity['layer'], layer)
            self.assertEqual(len(entity['points']), len(points))
            for actual, original in zip(entity['points'], points):
                for axis in (0, 1):
                    self.assertAlmostEqual(actual[axis], original[axis] + offset[axis], places=5)
        for sheet, displayed in zip(result['sheets'], sheets):
            offset = [min(p[i] for p in displayed) - min(p[i] for p in sheet['points']) for i in (0, 1)]
            match('SHEETS', sheet['points'], offset)
            for hole in sheet['holes']: match('HOLES', hole, offset)
            for part in sheet['parts']:
                self.assertEqual(part['rotation'], 90)
                match('PARTS', part['points'], offset)
                for hole in part['holes']: match('HOLES', hole, offset)
                center = [[(min(p[i] for p in part['points']) + max(p[i] for p in part['points'])) / 2
                           for i in (0, 1)]]
                match('LABELS', center, offset)
        self.assertEqual(cursor, len(entities))

    def test_svg_flag_overrides_json_without_enabling_svg_in_input(self):
        for configured in (False, True, 'json-preview.svg'):
            with self.subTest(configured=configured):
                path, data = self.job('first')
                data['output']['svg'] = configured
                path.write_text(json.dumps(data))
                preview = self.root / 'preview folder' / 'command line.svg'
                completed = subprocess.run([EXE, '--input', str(path), '--svg', str(preview)],
                                           cwd=self.root, capture_output=True, text=True, timeout=15)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                svg = ET.parse(preview).getroot()
                self.assertEqual(svg.tag, '{http://www.w3.org/2000/svg}svg')
                self.assertEqual(len(svg.findall('.//{http://www.w3.org/2000/svg}path')), 13)
                self.assertFalse((self.root / 'result.svg').exists())
                self.assertFalse((self.root / 'json-preview.svg').exists())

    def test_svg_and_dxf_flags_can_be_combined(self):
        path, _ = self.job('first')
        completed = subprocess.run([EXE, '--input', str(path), '--dxf', 'result.dxf', '--svg', 'result.svg'],
                                   cwd=self.root, capture_output=True, text=True, timeout=15)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue((self.root / 'result.dxf').is_file())
        ET.parse(self.root / 'result.svg')

    def test_json_svg_still_works_and_cli_paths_use_working_directory(self):
        path, data = self.job('first')
        data['output']['svg'] = 'json.svg'
        folder = self.root / 'input folder'
        folder.mkdir()
        path = folder / 'input.json'
        path.write_text(json.dumps(data))
        completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                   capture_output=True, text=True, timeout=15)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        ET.parse(folder / 'json.svg')
        completed = subprocess.run([EXE, '--input', str(path), '--svg', 'cli.svg'], cwd=self.root,
                                   capture_output=True, text=True, timeout=15)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        ET.parse(self.root / 'cli.svg')
        self.assertFalse((folder / 'cli.svg').exists())

    def test_svg_missing_filename_and_input_collision_are_rejected(self):
        path, data = self.job('first')
        for args in (['--svg'], ['--svg', str(path)]):
            completed = subprocess.run([EXE, '--input', str(path)] + args, cwd=self.root,
                                       capture_output=True, text=True, timeout=10)
            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(json.loads(path.read_text()), data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    options, remaining = parser.parse_known_args()
    EXE = str(Path(options.exe).resolve())
    unittest.main(argv=['cli_protocol.py'] + remaining, verbosity=2)
