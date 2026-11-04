#!/usr/bin/env python3
"""把「部署链路」的特征提取接到 microWakeWord 训练管线上。

背景（见 docs/06-kws-engine-p1.md）：同一个「你好小智」，走训练链路
（16k 原始 -> pymicro）模型给 255，走部署链路（afe -> 电平归一化 -> 8k→16k）
只给 105。模型不认识的是**这条信号链**。所以训练特征必须由固件同一份
C 代码产生 —— 就是 libkwsfeat.so。

用法：
    # 1) 冒烟：对若干 WAV 提特征，再用 TFLite 跑一遍，看概率对不对
    python mww_deploy_features.py check <wav> [<wav> ...]

    # 2) 在训练脚本里打补丁（替换 generate_features_for_clip）
    import mww_deploy_features as df
    df.patch()
"""
import ctypes
import os
import sys

import numpy as np
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))

# 训练片段前面要垫的真实房间底噪（设备实录，见 docs/08）。
# 为什么必须垫：afe 的去工频和电平归一化都带自适应状态，实测新起窗口的前
# 50 帧特征和连续流平均差 7.4（收敛后只剩 0.5），约 1.5 s 才稳定。
# 训练如果拿"冷启动窗口"的，部署却是"连续流"，模型看到的就是两个分布。
NOISE_RAW = os.path.expanduser("~/mww_data/noise30m.raw")
PREFIX_S = 2.0
_noise_cache = None
_rng = None
LIB_PATH = os.path.join(HERE, "libkwsfeat.so")
FEAT_DIM = 40
SCALE = 0.101960786  # 模型输入量化 scale（zp=-128）

_lib = None


def _bind():
    global _lib
    if _lib is not None:
        return _lib
    if not os.path.exists(LIB_PATH):
        raise SystemExit(f"找不到 {LIB_PATH}，先跑 make libkwsfeat.so")
    lib = ctypes.CDLL(LIB_PATH)
    lib.kwsfeat_reset.restype = None
    lib.kwsfeat_process.restype = ctypes.c_int
    lib.kwsfeat_process.argtypes = [
        ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
        ctypes.POINTER(ctypes.c_int8), ctypes.c_int,
    ]
    _lib = lib
    return lib


def _run(lib, pcm8k, out, total):
    """连续喂一段 8k PCM，返回新增帧数（不 reset，状态延续）。"""
    chunk = 320
    added = 0
    for i in range(0, len(pcm8k) - chunk + 1, chunk):
        n = lib.kwsfeat_process(
            pcm8k[i:i + chunk].ctypes.data_as(ctypes.POINTER(ctypes.c_int16)),
            chunk,
            out[total + added:].ctypes.data_as(ctypes.POINTER(ctypes.c_int8)),
            len(out) - total - added,
        )
        if n < 0:
            raise RuntimeError("kwsfeat 输出缓冲不足")
        added += n
    return added


def features_from_pcm8k(pcm8k: np.ndarray, prefix=None) -> np.ndarray:
    """8 kHz int16 -> (T, 40) float32，数值域与 pymicro 一致（0~27）。

    prefix: 可选的 8k 前置音频（真实底噪）。给了就先喂它让前级状态收敛，
    然后**只返回正片对应的帧**——这样训练特征和部署的连续流同分布。
    """
    lib = _bind()
    lib.kwsfeat_reset()

    pcm8k = np.ascontiguousarray(pcm8k.astype(np.int16))
    total_len = len(pcm8k) + (0 if prefix is None else len(prefix))
    # 8k 上采样 2 倍后，每 160 点（16k）出一帧 → 共 2×len/160 帧
    out = np.zeros((total_len // 80 + 16, FEAT_DIM), dtype=np.int8)

    n_pre = 0
    if prefix is not None and len(prefix) > 0:
        n_pre = _run(lib, np.ascontiguousarray(prefix.astype(np.int16)), out, 0)
    n_body = _run(lib, pcm8k, out, n_pre)

    # 反量化回 float：训练侧用 float，导出时再量化回同一个 int8（无损往返）
    body = out[n_pre:n_pre + n_body]
    return (body.astype(np.float32) + 128.0) * SCALE


def _noise_slice(n_samples: int, rng) -> np.ndarray:
    """从真机底噪里随机取一段，用作前级预热。"""
    global _noise_cache
    if _noise_cache is None:
        if not os.path.exists(NOISE_RAW):
            raise SystemExit(f"找不到真实底噪 {NOISE_RAW}（先跑 p21_noisecap 采集）")
        _noise_cache = np.fromfile(NOISE_RAW, dtype="<i2")
    bank = _noise_cache
    off = int(rng.integers(0, max(1, len(bank) - n_samples)))
    return bank[off:off + n_samples]


def features_from_audio16k(audio: np.ndarray, rng=None) -> np.ndarray:
    """16 kHz float[-1,1] -> (T, 40)。先降到 8 kHz 模拟相机麦克风带宽。

    rng 给定（训练时）会前置一段真实底噪预热；不给则按冷启动处理（只用于对照）。
    """
    pcm16 = np.clip(audio * 32768.0, -32768, 32767).astype(np.int16)
    pcm8 = np.clip(np.round(resample_poly(pcm16.astype(np.float32), 1, 2)),
                   -32768, 32767).astype(np.int16)
    prefix = None
    if rng is not None:
        prefix = _noise_slice(int(PREFIX_S * 8000), rng)
    return features_from_pcm8k(pcm8, prefix=prefix)


def generate_features_for_clip(audio_samples, step_ms=10):
    """替换 microwakeword.audio.audio_utils.generate_features_for_clip。

    参数是 **音频样本**（float32，[-1,1]，16 kHz），不是 Clip 对象
    —— 上游 audio_utils 里就是这个签名，spectrogram_generator 传的是
    augmenter 增强后的 ndarray。

    step_ms：训练默认 20 ms，但固件固定 10 ms（40 维 mel 的步进）。
    这里忽略它，需要时按整数倍抽稀，保证与部署一致。
    """
    if hasattr(audio_samples, "audio"):
        audio_samples = audio_samples.audio
    global _rng
    if _rng is None:
        import numpy as _np
        _rng = _np.random.default_rng(1234)
    feats = features_from_audio16k(np.asarray(audio_samples, dtype=np.float32), rng=_rng)
    if step_ms and step_ms > 10:
        # 与训练管线保持一致：step_ms=20 时每两帧取一帧
        feats = feats[:: max(1, step_ms // 10)]
    return feats


def patch():
    """就地替换训练管线里的特征提取函数。"""
    import microwakeword.audio.audio_utils as au
    import microwakeword.audio.spectrograms as sg

    au.generate_features_for_clip = generate_features_for_clip
    sg.generate_features_for_clip = generate_features_for_clip
    return True


def _check(paths):
    """对 WAV 提特征，再用 TFLite 跑一遍，打印峰值概率。"""
    import wave

    from ai_edge_litert.interpreter import Interpreter, OpResolverType

    model = os.path.expanduser(
        "~/mww_data/trained_models/wakeword_v2/tflite_stream_state_internal_quant/"
        "stream_state_internal_quant.tflite")
    interp = Interpreter(model_path=model,
                         experimental_op_resolver_type=OpResolverType.BUILTIN_REF)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]

    for p in paths:
        with wave.open(p, "rb") as w:
            assert w.getframerate() == 16000, f"{p} 需要 16k"
            x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        feats = features_from_audio16k(x.astype(np.float32) / 32768.0)

        q = np.clip(np.round(feats / SCALE - 128.0), -128, 127).astype(np.int8)
        try:
            interp.reset_all_variables()
        except Exception:
            pass
        peak = 0
        for i in range(0, len(q) - 2, 3):
            interp.set_tensor(inp["index"], q[i:i + 3].reshape(inp["shape"]))
            interp.invoke()
            peak = max(peak, int(interp.get_tensor(out["index"]).reshape(-1)[0]))

        print(f"{os.path.basename(p):34s} 帧={len(feats):3d}  峰值概率={peak:3d}")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] != "check":
        raise SystemExit(__doc__)
    _check(sys.argv[2:])
