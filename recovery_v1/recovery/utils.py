import numpy as np


def clamp(x: float, lo: float, hi: float) -> float:
    return float(min(max(x, lo), hi))


def ramp(x: float, a: float, b: float) -> float:
    """x 从 a 到 b 线性映射到 0→1（两端截断）。"""
    if b == a:
        return 1.0 if x >= b else 0.0
    return clamp((x - a) / (b - a), 0.0, 1.0)


def weighted_median(values, weights) -> float:
    v = np.asarray(values, float)
    w = np.asarray(weights, float)
    m = w > 0
    v, w = v[m], w[m]
    if v.size == 0:
        raise ValueError("empty")
    order = np.argsort(v)
    v, w = v[order], w[order]
    cw = np.cumsum(w)
    return float(v[np.searchsorted(cw, 0.5 * cw[-1])])


def round_half_up(x: float) -> int:
    return int(np.floor(x + 0.5))
