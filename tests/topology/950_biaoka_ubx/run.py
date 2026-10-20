#!/usr/bin/env python3
"""Fresh independent 950 acceptance; preserve all previous result directories."""
from pathlib import Path
import subprocess
import sys
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
subprocess.run([sys.executable, '-B', str(HERE.parent/'run_acceptance.py'),
                '--950-biaoka-ubx', '--output', str(HERE/'results'), *sys.argv[1:]],
               cwd=ROOT, check=True)
subprocess.run([sys.executable, '-B', str(HERE.parent/'report_acceptance.py'),
                '--output', str(HERE/'results')], cwd=ROOT, check=True)
