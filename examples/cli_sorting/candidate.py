"""Single-file sorting CLI; keep its stdin/stdout contract during evolution."""
import sys


def main():
    tokens = list(map(int, sys.stdin.read().split()))
    if not tokens or tokens[0] < 0 or len(tokens) != tokens[0] + 1:
        raise ValueError("expected N followed by N integers")
    values = tokens[1:]
    # EVOLVE-BLOCK-START
    for i in range(1, len(values)):
        value = values[i]
        j = i
        while j > 0 and values[j - 1] > value:
            values[j] = values[j - 1]
            j -= 1
        values[j] = value
    # EVOLVE-BLOCK-END
    print(*values)


if __name__ == "__main__":
    main()
