"""Offline protocol fixture: candidates add two integers from stdin."""
import json
import subprocess
import sys

request = json.load(sys.stdin)
try:
    result = subprocess.run(request["candidate"]["argv"], cwd=request["candidate"]["cwd"],
                            input="2 3\n", capture_output=True, text=True, timeout=2)
    passed = result.returncode == 0 and result.stdout.strip() == "5"
    response = {"protocol_version": 1, "passed": passed,
                "metrics": {"combined_score": -1} if passed else {}}
    if not passed:
        response["reason"] = "candidate_failed" if result.returncode else "wrong_answer"
except subprocess.TimeoutExpired:
    response = {"protocol_version": 1, "passed": False, "metrics": {}, "reason": "candidate_timeout"}
except OSError as error:
    print(str(error), file=sys.stderr)
    sys.exit(2)
json.dump(response, sys.stdout)
