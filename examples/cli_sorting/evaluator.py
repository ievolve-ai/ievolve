"""Protocol v1 evaluator. Candidate I/O and process startup count toward runtime."""
import json
import math
import os
from pathlib import Path
import selectors
import subprocess
import sys
import time

OUTPUT_LIMIT = 1024 * 1024


class Rejected(Exception):
    pass


def run_candidate(argv, cwd, payload, timeout):
    """Bound both streams while running; inherit the framework's process group."""
    start = time.monotonic()
    process = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    output = bytearray()
    total = 0
    offset = 0
    try:
        with selectors.DefaultSelector() as selector:
            for stream in (process.stdin, process.stdout, process.stderr):
                os.set_blocking(stream.fileno(), False)
            if payload:
                selector.register(process.stdin, selectors.EVENT_WRITE)
            else:
                process.stdin.close()
            selector.register(process.stdout, selectors.EVENT_READ)
            selector.register(process.stderr, selectors.EVENT_READ)
            while selector.get_map() or process.poll() is None:
                remaining = timeout - (time.monotonic() - start)
                if remaining <= 0:
                    raise Rejected("candidate_timeout")
                for key, _ in selector.select(min(remaining, 0.05)):
                    stream = key.fileobj
                    if stream is process.stdin:
                        try:
                            offset += os.write(stream.fileno(), payload[offset:offset + 4096])
                        except BrokenPipeError:
                            offset = len(payload)
                        except BlockingIOError:
                            continue
                        if offset == len(payload):
                            selector.unregister(stream)
                            stream.close()
                    else:
                        try:
                            chunk = os.read(stream.fileno(), 4096)
                        except BlockingIOError:
                            continue
                        if not chunk:
                            selector.unregister(stream)
                            stream.close()
                            continue
                        total += len(chunk)
                        if total > OUTPUT_LIMIT:
                            raise Rejected("candidate_output_limit")
                        if stream is process.stdout:
                            output.extend(chunk)
            return process.returncode, bytes(output), (time.monotonic() - start) * 1000
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        for stream in (process.stdin, process.stdout, process.stderr):
            stream.close()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate request key")
        result[key] = value
    return result


def validate_request(request):
    if (not isinstance(request, dict)
            or set(request) != {"protocol_version", "candidate", "limits", "enable_artifacts"}
            or type(request["protocol_version"]) is not int or request["protocol_version"] != 1
            or type(request["enable_artifacts"]) is not bool):
        raise ValueError("invalid protocol v1 request")
    candidate = request["candidate"]
    if not isinstance(candidate, dict) or set(candidate) != {"argv", "cwd"}:
        raise ValueError("invalid candidate description")
    argv = candidate["argv"]
    cwd = candidate["cwd"]
    if (not isinstance(argv, list) or not argv or not argv[0]
            or any(not isinstance(arg, str) or "\0" in arg for arg in argv)
            or not isinstance(cwd, str) or "\0" in cwd or not Path(cwd).is_absolute()
            or not Path(cwd).is_dir()):
        raise ValueError("invalid candidate command")
    limits = request["limits"]
    if not isinstance(limits, dict) or set(limits) != {"evaluation_timeout_ms", "max_artifact_bytes"}:
        raise ValueError("invalid limits")
    if (type(limits["evaluation_timeout_ms"]) is not int or not 0 < limits["evaluation_timeout_ms"] <= 2**63 - 1
            or type(limits["max_artifact_bytes"]) is not int or not 0 <= limits["max_artifact_bytes"] <= 2**63 - 1):
        raise ValueError("invalid limits")


def cases():
    state = 42
    random_values = []
    for _ in range(384):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        random_values.append(state % 2001 - 1000)
    return [[], [7], [3, 1, 2], [2, -1, 2, 0, -1], list(range(128)),
            list(range(383, -1, -1)), random_values]


def evaluate_request(request):
    validate_request(request)
    deadline = time.monotonic() + request["limits"]["evaluation_timeout_ms"] / 1000
    candidate = request["candidate"]

    def run_case(values):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Rejected("evaluation_timeout")
        payload = (str(len(values)) + "\n" + " ".join(map(str, values)) + "\n").encode()
        code, output, duration = run_candidate(candidate["argv"], candidate["cwd"], payload, min(2, remaining))
        if code:
            raise Rejected("candidate_exit")
        try:
            actual = [int(token) for token in output.decode("utf-8").split()]
        except (ValueError, UnicodeError):
            raise Rejected("invalid_output") from None
        if actual != sorted(values):
            raise Rejected("wrong_answer")
        return duration

    try:
        inputs = cases()
        for values in inputs:
            run_case(values)
        run_case(inputs[-1])  # Warmup after correctness passes.
        runtime_ms = sum(run_case(inputs[-1]) for _ in range(5)) / 5
        score = 1 / max(runtime_ms, 0.001)
        if not math.isfinite(score):
            raise ValueError("nonfinite measurement")
        result = {"protocol_version": 1, "passed": True,
                  "metrics": {"combined_score": score, "runtime_ms": runtime_ms}}
    except Rejected as error:
        result = {"protocol_version": 1, "passed": False, "reason": str(error), "metrics": {}}
    if request["enable_artifacts"]:
        summary = "Seven correctness cases, one warmup, five measured CLI invocations."
        result["artifacts"] = {"summary": summary[:request["limits"]["max_artifact_bytes"]]}
    return result


def main():
    try:
        text = sys.stdin.buffer.read(1024 * 1024 + 1)
        if len(text) > 1024 * 1024:
            raise ValueError("request too large")
        request = json.loads(text, object_pairs_hook=unique_object)
        result = evaluate_request(request)
        json.dump(result, sys.stdout, allow_nan=False)
        return 0
    except Exception as error:
        # Missing executable, malformed request and evaluator bugs stop the run.
        print(f"Evaluator infrastructure error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
