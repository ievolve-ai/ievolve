import time
import numpy as np

_START = time.monotonic()


def run_packing():
    """Construct 26 circles; maximize total radius with deterministic restarts."""
    n = 26
    deadline = min(_START + 52.0, time.monotonic() + 49.0)
    rng = np.random.default_rng(20260929)
    ii, jj = np.triu_indices(n, 1)
    pair_rows = np.arange(len(ii))
    margin = 2e-9

    def repair(c, r):
        c = np.clip(np.asarray(c).reshape(n, 2), 1e-7, 1 - 1e-7)
        r = np.maximum(np.asarray(r).reshape(n), 0.0)
        walls = np.minimum(c, 1 - c).min(axis=1)
        r = np.minimum(r, np.maximum(walls - margin, 0.0))
        distances = np.linalg.norm(c[ii] - c[jj], axis=1)
        sums = r[ii] + r[jj]
        positive = sums > 0
        if np.any(positive):
            scale = min(
                1.0,
                float(np.min(
                    np.maximum(distances[positive] - margin, 0.0)
                    / sums[positive]
                )),
            )
            r = r * scale
        return c.copy(), r.copy()

    # Explicit feasible fallback, also useful as an optimization seed.
    small = 0.1 * (3 - 2 * np.sqrt(2))
    c = np.array([
        ((x + 0.5) / 5, (y + 0.5) / 5)
        for y in range(5) for x in range(5)
    ] + [(small, small)])
    r = np.r_[np.full(25, 0.1), small]
    best_c, best_r = repair(c, r)
    best_sum = float(best_r.sum())

    try:
        from scipy.optimize import minimize
    except ImportError:
        return best_c, best_r, best_sum

    class TimeLimit(Exception):
        pass

    def check_time():
        if time.monotonic() >= deadline:
            raise TimeLimit

    objective_jac = np.r_[np.zeros(2 * n), -np.ones(n)]
    fixed_jac = np.zeros((4 * n + len(ii), 3 * n))
    k = np.arange(n)
    for block, axis, sign in (
        (0, 0, 1), (1, 1, 1), (2, 0, -1), (3, 1, -1)
    ):
        fixed_jac[block * n + k, 2 * k + axis] = sign
        fixed_jac[block * n + k, 2 * n + k] = -1
    rows = 4 * n + pair_rows
    fixed_jac[rows, 2 * n + ii] = -1
    fixed_jac[rows, 2 * n + jj] = -1

    def constraints(z):
        check_time()
        c = z[:2 * n].reshape(n, 2)
        r = z[2 * n:]
        distances = np.linalg.norm(c[ii] - c[jj], axis=1)
        return np.concatenate((
            c[:, 0] - r,
            c[:, 1] - r,
            1 - c[:, 0] - r,
            1 - c[:, 1] - r,
            distances - r[ii] - r[jj],
        )) - 2e-8

    def jacobian(z):
        check_time()
        c = z[:2 * n].reshape(n, 2)
        delta = c[ii] - c[jj]
        lengths = np.maximum(np.linalg.norm(delta, axis=1), 1e-15)
        unit = delta / lengths[:, None]
        jac = fixed_jac.copy()
        jac[rows, 2 * ii] = unit[:, 0]
        jac[rows, 2 * ii + 1] = unit[:, 1]
        jac[rows, 2 * jj] = -unit[:, 0]
        jac[rows, 2 * jj + 1] = -unit[:, 1]
        return jac

    # Keep several distinct local optima for subsequent exploration.
    archive = []

    def retain(z):
        nonlocal best_c, best_r, best_sum
        if np.all(np.isfinite(z)):
            c, r = repair(z[:2 * n], z[2 * n:])
            score = float(r.sum())
            if score > best_sum:
                best_c, best_r, best_sum = c, r, score

    def callback(z):
        retain(z)
        check_time()

    def row_seed(counts):
        c = np.array([
            ((x + 0.5) / count, (y + 0.5) / len(counts))
            for y, count in enumerate(counts)
            for x in range(count)
        ])
        distances = np.linalg.norm(c[:, None] - c[None, :], axis=2)
        np.fill_diagonal(distances, np.inf)
        r = np.minimum(
            np.minimum(c, 1 - c).min(axis=1),
            0.5 * distances.min(axis=1),
        )
        return c, r

    seeds = [(best_c.copy(), best_r.copy())]
    seeds.extend(row_seed(counts) for counts in (
        (5, 5, 6, 5, 5),
        (5, 6, 5, 5, 5),
        (6, 5, 4, 5, 6),
        (4, 5, 4, 5, 4, 4),
    ))
    extra_seeds = [
        row_seed(counts) for counts in (
            (4, 5, 4, 4, 5, 4),
            (5, 4, 4, 4, 4, 5),
            (5, 4, 5, 4, 4, 4),
            (6, 4, 6, 4, 6),
        )
    ]
    bounds = [(1e-7, 1 - 1e-7)] * (2 * n) + [(1e-8, 0.5)] * n

    try:
        for attempt in range(80):
            check_time()
            if attempt < len(seeds):
                c, r = seeds[attempt]
            elif attempt < 17:
                c, r = best_c.copy(), best_r.copy()
                amplitude = (0.012, 0.025, 0.045, 0.07)[
                    (attempt - len(seeds)) % 4
                ]
                c = np.clip(c + rng.normal(0, amplitude, c.shape),
                            0.015, 0.985)
                r *= 0.96
            elif attempt < 21:
                c, r = extra_seeds[attempt - 17]
            else:
                if archive and attempt % 4 == 0:
                    _, c, r = archive[(attempt // 4) % len(archive)]
                    c, r = c.copy(), r.copy()
                else:
                    c, r = best_c.copy(), best_r.copy()

                mode = (attempt - 21) % 6
                if mode < 4:
                    amplitude = (0.008, 0.022, 0.045, 0.085)[mode]
                    c += rng.normal(0, amplitude, c.shape)
                elif mode == 4:
                    # Change a local neighborhood without disrupting all rows.
                    anchor = int(rng.integers(n))
                    near = np.argsort(
                        np.linalg.norm(c - c[anchor], axis=1)
                    )[:7]
                    c[near] += rng.normal(0, 0.065, (len(near), 2))
                else:
                    # Relocate a small circle into a promising unfilled gap.
                    index = int(np.argsort(r)[int(rng.integers(5))])
                    candidates = rng.uniform(0.035, 0.965, (256, 2))
                    clearance = np.minimum(
                        candidates, 1 - candidates
                    ).min(axis=1)
                    others = np.arange(n) != index
                    clearance = np.minimum(
                        clearance,
                        (
                            np.linalg.norm(
                                candidates[:, None] - c[None, others],
                                axis=2,
                            ) - r[None, others]
                        ).min(axis=1),
                    )
                    chosen = int(np.argmax(clearance))
                    c[index] = candidates[chosen]
                    r[index] = max(0.005, clearance[chosen])
                c = np.clip(c, 0.015, 0.985)
                r *= 0.96

            result = minimize(
                lambda z: -float(z[2 * n:].sum()),
                np.r_[c.ravel(), r],
                jac=lambda z: objective_jac,
                method="SLSQP",
                bounds=bounds,
                constraints={
                    "type": "ineq",
                    "fun": constraints,
                    "jac": jacobian,
                },
                callback=callback,
                options={"maxiter": 220, "ftol": 2e-10, "disp": False},
            )
            retain(result.x)
            if np.all(np.isfinite(result.x)):
                c, r = repair(result.x[:2 * n], result.x[2 * n:])
                score = float(r.sum())
                if not any(abs(score - entry[0]) < 2e-6 for entry in archive):
                    archive.append((score, c, r))
                    archive.sort(key=lambda entry: entry[0], reverse=True)
                    del archive[6:]
    except TimeLimit:
        pass

    best_c, best_r = repair(best_c, best_r)
    return best_c, best_r, float(best_r.sum())


def construct_packing():
    return run_packing()


if __name__ == "__main__":
    centers, radii, reported_sum = run_packing()
    print(f"Sum of radii: {reported_sum:.12f}")