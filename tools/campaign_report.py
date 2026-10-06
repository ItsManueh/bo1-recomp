"""Summary of a campaign sweep (tools/campaign_sweep.ps1): per mission, whether the map loaded,
engine and runtime errors, frame rate after loading and the worst hitches.

Usage: python tools/campaign_report.py [logs_folder]
"""
import glob
import os
import re
import sys

logs = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'logs')
FPS = re.compile(r'average FPS over the last \d+ s = ([\d.]+) \| worst frame ([\d.]+) ms')
MISSIONS = ('int_escape cuba vorkuta pentagon flashpoint khe_sanh hue_city kowloon fullahead creek_1 '
            'river wmd_sr71 wmd pow rebirth underwaterbase terminal outro').split()
for path in sorted(glob.glob(os.path.join(logs, 'sweep_*.log')), key=os.path.getmtime):
    name = os.path.basename(path)[len('sweep_'):-len('.log')]
    if name not in MISSIONS:
        continue  # logs of other sweeps
    loaded = False
    errors = []
    fps = []
    hitches = 0
    for line in open(path, encoding='utf-8', errors='replace'):
        if f'bo1: map {name}' in line:
            loaded = True
        if 'engine error' in line or '[error]' in line:
            errors.append(line.split('] ', 4)[-1].strip()[:110])
        if loaded:
            m = FPS.search(line)
            if m:
                fps.append((float(m.group(1)), float(m.group(2))))
            if 'bo1: hitch' in line:
                hitches += 1
    # Skip the loading screen: frame rate of the stats lines after the first one past the load.
    steady = fps[2:] if len(fps) > 3 else fps
    avg = sum(f for f, _ in steady) / len(steady) if steady else 0
    worst = max((w for _, w in steady), default=0)
    print(f'{name:16} loaded={"yes" if loaded else "NO ":3}  fps {avg:5.1f}  worst {worst:7.1f} ms'
          f'  hitches {hitches:3}  errors {len(errors)}')
    for e in sorted(set(errors))[:5]:
        print(f'    ! {e}')
