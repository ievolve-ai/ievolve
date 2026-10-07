"""Offline CLI evaluation: run the candidate and score its numeric stdout."""
import json
from pathlib import Path
import subprocess
import sys

request = json.load(sys.stdin)
try:
    run = subprocess.run(request['candidate']['argv'], cwd=request['candidate']['cwd'],
                         capture_output=True, text=True, timeout=5)
except OSError as error:
    print(error, file=sys.stderr)
    sys.exit(2)
try:
    score = float(run.stdout.strip())
    if run.returncode:
        raise ValueError('candidate exited nonzero')
    response = {'protocol_version': 1, 'passed': True, 'metrics': {'combined_score': score},
                'artifacts': {'source': Path(request['candidate']['argv'][-1]).read_text(),
                              'binary': {'__bytes__': 'AP8='}}}
except ValueError:
    response = {'protocol_version': 1, 'passed': False, 'reason': 'invalid_candidate_output', 'metrics': {}}
json.dump(response, sys.stdout)
