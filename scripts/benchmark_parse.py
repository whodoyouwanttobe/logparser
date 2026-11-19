#!/usr/bin/env python3
"""Measure CLI parse (file reading + parsing + formatting), NOT database import."""
import argparse
import csv
import datetime as dt
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', default='./logparser')
    p.add_argument('--rows', type=int, nargs='+', default=[10000, 100000, 1000000])
    p.add_argument('--runs', type=int, default=5)
    p.add_argument('--output', default='benchmark-parse.csv')
    a = p.parse_args()
    if a.runs < 1 or any(n < 1 for n in a.rows):
        p.error('rows and runs must be positive')
    binary = str(Path(a.binary).resolve())
    if not os.access(binary, os.X_OK):
        p.error('build the executable first: make')
    output = Path(a.output)
    if output.exists():
        p.error('output exists; choose a new path to preserve measurements')
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        sha = 'unknown'
    with tempfile.TemporaryDirectory(prefix='logparser-bench-') as tmp, output.open('x', newline='') as out:
        writer = csv.writer(out)
        writer.writerow(['rows','bytes','run','seconds','median_seconds','sha','platform','date_utc','scope'])
        for count in a.rows:
            log = Path(tmp)/'input.log'
            with log.open('w') as f:
                for i in range(count):
                    f.write(f'2026-11-18T12:00:00+07:00 bench app[1]: [INFO] trace_id=bench event={i}\n')
            def run():
                start = time.perf_counter()
                # CLI parse limits output to 20 lines by default; 0 means the whole file.
                subprocess.run([binary,'parse','-n','0',str(log)], stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, check=True)
                return time.perf_counter()-start
            run()  # warm-up; excluded from results
            times = [run() for _ in range(a.runs)]
            for i, elapsed in enumerate(times,1):
                writer.writerow([count,log.stat().st_size,i,f'{elapsed:.6f}',
                    f'{statistics.median(times):.6f}',sha,platform.platform(),
                    dt.datetime.now(dt.timezone.utc).isoformat(),'CLI parse; no DB; warm cache'])
            out.flush()
            print(f'{count} rows: median {statistics.median(times):.6f} s')


if __name__ == '__main__':
    main()
