"""Exercise the executable's file protocol without requiring FreeCAD."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import uuid


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
        self.process = subprocess.Popen([EXE, '--input', str(path)], cwd=self.root,
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
        latest = json.loads((self.root / 'result.json').read_text(encoding='utf-8'))
        self.assertEqual(latest, json.loads(snapshots[-1].read_text(encoding='utf-8')))
        self.assertFalse(list(self.root.rglob('*.tmp')))

    def test_cancel_path_cannot_overwrite_input(self):
        path, data = self.job('first')
        data['output']['cancelFile'] = 'input.json'
        path.write_text(json.dumps(data), encoding='utf-8')
        completed = subprocess.run([EXE, '--input', str(path)], cwd=self.root,
                                   capture_output=True, text=True, timeout=10)
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(json.loads(path.read_text(encoding='utf-8')), data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    options, remaining = parser.parse_known_args()
    EXE = str(Path(options.exe).resolve())
    unittest.main(argv=['workbench_protocol.py'] + remaining, verbosity=2)
