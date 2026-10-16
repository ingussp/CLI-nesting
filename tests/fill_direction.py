"""Public JSON direction settings and world-coordinate CPU/GPU layouts."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


class DirectionTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='nesting direction ')
        self.root=Path(self.temp.name)

    def tearDown(self): self.temp.cleanup()

    def run_job(self,direction,sheet=None,mode='first',gpu=False,quantity=3,margin=0,part=None):
        data=dict(units='mm',config=dict(mode=mode,fillDirection=direction,threads=1,resolution=1,
                  timeLimitSeconds=(2 if gpu else .5) if mode=='continuous' else 0,gpu=dict(enabled=gpu,fallbackToCpu=False),
                  partToSheet=margin),sheets=[sheet or dict(width=30,height=100)],
                  parts=part if isinstance(part,list) else [part or dict(points=[[0,0],[10,0],[10,10],[0,10]],quantity=quantity,allowedAngles=[0])],
                  output=dict(json='result.json'))
        path=self.root/'input.json';path.write_text(json.dumps(data))
        completed=subprocess.run([EXE,'--input',str(path)],cwd=self.root,capture_output=True,text=True,timeout=20)
        self.assertEqual(completed.returncode,0,completed.stderr)
        result=json.loads((self.root/'result.json').read_text())
        self.assertEqual(result['fillDirection'],direction)
        self.assertGreaterEqual(result['fillDirectionCost'],0)
        if VALIDATOR:
            validated=subprocess.run([VALIDATOR,str(path),str(self.root/'result.json')],capture_output=True,text=True,timeout=10)
            self.assertEqual(validated.returncode,0,validated.stderr+validated.stdout)
        return result

    def test_rectangles_fill_from_short_or_long_edge(self):
        for mode in ('first','continuous'):
            for width,height in ((30,100),(100,30)):
                for direction in ('narrow-to-wide','wide-to-narrow'):
                    with self.subTest(mode=mode,dimensions=(width,height),direction=direction):
                        r=self.run_job(direction,dict(width=width,height=height),mode)
                        self.assertEqual(r['placed'],3)
                        p=[p for s in r['sheets'] for p in s['parts']]
                        dx=max(v[0] for a in p for v in a['points'])-min(v[0] for a in p for v in a['points'])
                        dy=max(v[1] for a in p for v in a['points'])-min(v[1] for a in p for v in a['points'])
                        horizontal=(width<height)==(direction=='narrow-to-wide')
                        self.assertEqual((dx,dy),(30,10) if horizontal else (10,30))

    def test_tapered_stock_in_both_world_orientations(self):
        base=[[0,0],[60,0],[35,100],[25,100]]
        for reverse in (False,True):
            points=[[x-70,(100-y if reverse else y)+120] for x,y in base]
            for direction in ('narrow-to-wide','wide-to-narrow'):
                with self.subTest(reverse=reverse,direction=direction):
                    part=dict(points=[[0,0],[4,0],[4,4],[0,4]],quantity=3,allowedAngles=[0])
                    r=self.run_job(direction,dict(points=points),margin=1,part=part)
                    self.assertEqual(r['placed'],3)
                    edge=r['sheets'][0]['fillStartEdge']
                    expected_top=(direction=='narrow-to-wide')!=reverse
                    self.assertEqual(edge,'top' if expected_top else 'bottom')
                    ys=[y for s in r['sheets'] for p in s['parts'] for x,y in p['points']]
                    self.assertGreater(min(ys),200) if expected_top else self.assertLess(max(ys),140)

    def test_holes_rotations_and_multiple_stocks(self):
        sheet=dict(points=[[10,20],[40,20],[40,120],[10,120]],holes=[[[20,60],[30,60],[30,80],[20,80]]],quantity=2)
        part=dict(points=[[2,3],[10,3],[10,7],[2,7]],quantity=40,allowedAngles=[90])
        for direction in ('narrow-to-wide','wide-to-narrow'):
            r=self.run_job(direction,sheet,part=part,margin=1)
            self.assertEqual(r['placed'],40)
            self.assertTrue(all(p['rotation']==90 for s in r['sheets'] for p in s['parts']))

    def test_large_and_small_panels_advance_to_more_stock(self):
        parts=[]
        for i in range(4):
            w=1250+10*i
            parts.append(dict(points=[[0,0],[w,0],[w,1800],[0,1800]],quantity=1,allowedAngles=[0,90,180,270]))
        for i in range(12):
            w=200+i
            parts.append(dict(points=[[0,0],[w,0],[w,250],[0,250]],quantity=1,allowedAngles=[0,90,180,270]))
        for direction in ('narrow-to-wide','wide-to-narrow'):
            r=self.run_job(direction,dict(width=2000,height=2800,quantity=4),part=parts)
            self.assertEqual(r['placed'],16)

    def test_gpu_preserves_cpu_order(self):
        devices=subprocess.run([EXE,'--list-gpus'],capture_output=True,text=True,timeout=10)
        if devices.returncode or 'NVIDIA' not in devices.stdout: self.skipTest('No NVIDIA OpenCL test device')
        for mode in ('first','continuous'):
            for direction in ('narrow-to-wide','wide-to-narrow'):
                cpu=self.run_job(direction,mode=mode)
                gpu=self.run_job(direction,mode=mode,gpu=True)
                self.assertEqual(cpu['sheets'],gpu['sheets'])
                self.assertTrue(gpu['gpu']['used'])
        # Cover reverse traversal from top and right as well as axis exchange.
        for sheet in (dict(points=[[0,0],[60,0],[35,100],[25,100]]),
                      dict(points=[[0,0],[100,25],[100,35],[0,60]])):
            for direction in ('narrow-to-wide','wide-to-narrow'):
                cpu=self.run_job(direction,sheet=sheet)
                gpu=self.run_job(direction,sheet=sheet,gpu=True)
                self.assertEqual(cpu['sheets'],gpu['sheets'])
                recursive=self.run_job(direction,sheet=sheet,gpu=True,mode='continuous')
                self.assertEqual(recursive['placed'],3)
                self.assertTrue(recursive['gpu']['used'])

    def test_unknown_directions_are_rejected(self):
        for direction in ('automatic','reverse',1,None):
            data=dict(config=dict(fillDirection=direction),sheets=[dict(width=30,height=100)],
                      parts=[dict(points=[[0,0],[1,0],[1,1],[0,1]])])
            path=self.root/'bad.json';path.write_text(json.dumps(data))
            r=subprocess.run([EXE,'--input',str(path)],capture_output=True,text=True,timeout=5)
            self.assertNotEqual(r.returncode,0)
            self.assertIn('fillDirection must be',r.stderr)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--exe',required=True);p.add_argument('--validator')
    args,remaining=p.parse_known_args();EXE=str(Path(args.exe).resolve());VALIDATOR=str(Path(args.validator).resolve()) if args.validator else None
    unittest.main(argv=['fill_direction']+remaining)
