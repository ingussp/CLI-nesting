"""Exercise the executable's file protocol without requiring FreeCAD."""
import argparse
import json
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
        job_id = str(uuid.uuid4())
        data = dict(job_id=job_id, units='mm',
                    config=dict(mode=mode, threads=2, continuousRoundSeconds=.1,
                                timeLimitSeconds=.1 if mode == 'timed' else 0),
                    sheets=[dict(points=[[0, 0], [100, 0], [100, 100], [0, 100]], quantity=1)],
                    parts=[dict(id='part_0', points=[[0, 0], [10, 0], [10, 10], [0, 10]],
                                quantity=12, rotations=1)],
                    output=dict(json='result.json', cancelFile='.cancel-' + job_id))
        path = self.root / 'input.json'
        path.write_text(json.dumps(data), encoding='utf-8')
        return path, data

    def test_first_and_timed_echo_job(self):
        for mode in ('first', 'timed'):
            path, data = self.job(mode)
            completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                       capture_output=True, text=True, timeout=15)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            result = json.loads((self.root / 'result.json').read_text(encoding='utf-8'))
            self.assertEqual(result['job_id'], data['job_id'])
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
        self.assertEqual(result['job_id'], data['job_id'])
        self.assertFalse((history / 'obsolete.json').exists())
        self.assertIsNone(self.process.poll(), 'Continuous search must stay running')
        (self.root / data['output']['cancelFile']).write_text(data['job_id'])
        _, errors = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 0, errors)
        snapshots = sorted(history.glob('result*.json'), key=lambda p: int(p.stem[6:]))
        self.assertTrue(snapshots)
        for snapshot in snapshots:
            self.assertTrue(snapshot.with_suffix('.dxf').is_file())
            ET.parse(snapshot.with_suffix('.svg'))
        latest = json.loads((self.root / 'result.json').read_text(encoding='utf-8'))
        self.assertEqual(latest, json.loads(snapshots[-1].read_text(encoding='utf-8')))
        self.assertFalse(list(self.root.rglob('*.tmp')))
        self.assertEqual(latest['stopReason'], 'searching')
        log = (self.root / 'recursive-progress.log').read_text()
        self.assertIn('New job:', log)
        self.assertIn('Recursive finished:', log)
        self.assertIn('cancelled=1', log)

    def test_cancel_path_cannot_overwrite_input(self):
        path, data = self.job('first')
        data['output']['cancelFile'] = 'input.json'
        path.write_text(json.dumps(data), encoding='utf-8')
        completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                   capture_output=True, text=True, timeout=10)
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(json.loads(path.read_text(encoding='utf-8')), data)

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
    unittest.main(argv=['workbench_protocol.py'] + remaining, verbosity=2)
