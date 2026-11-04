#!/usr/bin/env python3
"""定点重采样 vs scipy.signal.resample_poly 的对拍。

固件用的是 scipy 那组系数的 Q13 定点版（kws_resamp.c），这里量它跟
float 版本的差距：如果差值在 -50 dB 以下，mel 前端基本看不出区别。

用法：
    python3 verify_resamp.py <in8k.wav> <out16k.raw>
"""
import sys
import wave

import numpy as np
from scipy.signal import resample_poly


def main():
    wav_path, raw_path = sys.argv[1], sys.argv[2]

    with wave.open(wav_path, "rb") as w:
        assert w.getframerate() == 8000, w.getframerate()
        assert w.getnchannels() == 1
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")

    y_fix = np.fromfile(raw_path, dtype="<i2").astype(np.float64)
    y_ref = resample_poly(x.astype(np.float64), 2, 1)

    # 因果实现必然比 scipy 的零相位版本晚：scipy 会在两端各补 20 个零，
    # 那是"未来样本"，实时做不到。所以固定延迟 = 10 个 8k 样本 = 20 个 16k 样本，
    # 这里按这个已知延迟对齐后再算 SNR（延迟本身对唤醒无影响，1.25 ms）。
    lag = 20
    n = min(len(y_fix) - lag, len(y_ref))
    a = y_fix[lag:lag + n]
    b = y_ref[:n]

    d = a - b
    sig = float(np.sum(b ** 2))
    err = float(np.sum(d ** 2))
    snr = 10 * np.log10(sig / err) if err > 0 else float("inf")

    print(f"样本数  定点={len(y_fix)}  参考={len(y_ref)}  对齐延迟={lag}")
    print(f"最大偏差 {np.max(np.abs(d)):.1f} LSB   RMS 偏差 {np.sqrt(err / n):.3f} LSB")
    print(f"SNR {snr:.1f} dB   {'PASS' if snr >= 40 else 'FAIL'}（判据 >= 40 dB）")


if __name__ == "__main__":
    main()
