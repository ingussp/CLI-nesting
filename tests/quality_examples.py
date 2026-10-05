"""Customer-layout regressions; validate output with the exhaustive C++ predicates."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--validator', required=True)
    args = parser.parse_args()
    fixtures = Path(__file__).parent / 'fixtures'
    with tempfile.TemporaryDirectory(prefix='nesting quality ') as temp:
        root = Path(temp)
        for name in ('zvaigzne', 'plaukts', 'aplis'):
            job = json.loads((fixtures / f'{name}.json').read_text())
            layouts = []
            for threads in (1, 12):
                data = copy.deepcopy(job)
                data['config']['threads'] = threads
                input_path, output_path = root / 'input.json', root / 'result.json'
                input_path.write_text(json.dumps(data))
                subprocess.run([args.exe, '--input', str(input_path), '--output', str(output_path)],
                               check=True, capture_output=True, timeout=60)
                result = json.loads(output_path.read_text())
                subprocess.run([args.validator, str(input_path), str(output_path)], check=True, timeout=60)
                ids = [p['id'] for s in result['sheets'] for p in s['parts']]
                assert len(ids) == len(set(ids)) == result['placed'], 'Lost or duplicated placed copy'
                assert result['placed'] + result['unplacedCount'] == sum(p['quantity'] for p in data['parts'])
                if name == 'zvaigzne':
                    assert result['placed'] >= 89, result['placed']
                    assert max(y for p in result['sheets'][0]['parts'] for x, y in p['points']) > 2700
                    assert result['sheetRefill']['placements'] > 0
                elif name == 'plaukts':
                    counts = [len(s['parts']) for s in result['sheets']]
                    assert result['placed'] == 50 and len(counts) <= 4, counts
                    assert counts[0] >= 12 and counts[1] >= 10, counts
                    assert counts[-1] <= 7, counts
                else:
                    assert result['placed'] == 16 and result['holePlacements'] >= 8, result['holePlacements']
                layouts.append(result['sheets'])
            assert layouts[0] == layouts[1], f'{name}: thread count changed the layout'
            print(name, 'quality, clearance and 1/12-thread parity passed', flush=True)


if __name__ == '__main__':
    main()
