#!/usr/bin/env python3
"""公开语料 / 合成语音 → 固件同分布特征的公共工具。

三条硬规矩（见本地训练笔记，仓库不分发）：
  1. 特征一律走 libkwsfeat.so（mww_deploy_features），与相机上的
     afe → 电平归一化 → 8k→16k → mel 是同一份 C 代码。
  2. training / validation / testing 按「说话人」分组，测试集里的人训练时没出现过。
  3. 所有随机性来自显式 seed，manifest 记录参数，结果可复现。

注意：训练管线 gen_features_deploy.py 的 Augmentation 本身会加 RIR 与背景噪声
（概率 0.5 / 0.75）。本模块的 rir / noise 开关默认关闭，避免双重增强；
只有在不走那条管线、直接产出特征时才需要打开。
"""
import io
import json
import os
import shutil
import sys
import wave

import numpy as np
from scipy.signal import fftconvolve, resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mww_deploy_features as df  # noqa: E402

WIN_FRAMES = 309          # 3.2 s @ 10 ms，与 gen_noise_features.py 一致
MIN_FRAMES = 150          # 训练管线要求的最短样本
HOP_FRAMES = WIN_FRAMES // 2
FEAT_DIM = 40


def setup_deploy(noise_raw, lib_path=None):
    """显式指定真机底噪路径，不依赖 ~/mww_data 这个历史软链。

    libkwsfeat.so 是本机编译产物（.gitignore 忽略、不入库）：默认找脚本同目录，
    可用 KWSFEAT_LIB 环境变量或 lib_path 指向别处（如私有仓库里编好的那份）。
    构建方式：在 src/kws/test 下 make libkwsfeat.so。
    """
    path = os.path.expanduser(noise_raw)
    if not os.path.isfile(path):
        raise FileNotFoundError("找不到真机底噪: %s" % path)
    df.NOISE_RAW = path
    df._noise_cache = None
    override = lib_path or os.environ.get("KWSFEAT_LIB")
    if override:
        p = os.path.expanduser(override)
        if not os.path.isfile(p):
            raise FileNotFoundError("找不到 libkwsfeat.so: %s" % p)
        df.LIB_PATH = p
        df._lib = None
    df._bind()
    return path


def read_wav16k(src):
    """path 或 file-like → 16 kHz 单声道 float32 [-1,1]。"""
    if hasattr(src, "read"):
        w = wave.open(src, "rb")
    else:
        w = wave.open(os.path.expanduser(src), "rb")
    with w:
        rate, ch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise ValueError("只支持 16-bit PCM WAV，实际 sampwidth=%d" % sw)
    x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    if rate != 16000:
        g = np.gcd(rate, 16000)
        x = resample_poly(x, 16000 // g, rate // g)
    return np.clip(np.asarray(x, dtype=np.float32), -1.0, 1.0)


def write_wav16k(path, audio):
    x = np.clip(np.asarray(audio, dtype=np.float32), -1.0, 1.0)
    pcm = np.round(x * 32767.0).astype("<i2")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm.tobytes())


def load_rirs(rir_dir, limit=0):
    import glob
    files = sorted(glob.glob(os.path.join(os.path.expanduser(rir_dir), "*.wav")))
    if limit:
        files = files[:limit]
    return [(os.path.basename(f), read_wav16k(f)) for f in files]


def apply_rir(audio, rir):
    """16 kHz 域卷积，再归一化回原峰值（避免能量忽大忽小）。"""
    y = fftconvolve(audio, rir, mode="full")[:len(audio)]
    peak = float(np.max(np.abs(y)))
    src_peak = float(np.max(np.abs(audio)))
    if peak > 1e-8 and src_peak > 1e-8:
        y = y * (src_peak / peak)
    return np.clip(np.asarray(y, dtype=np.float32), -1.0, 1.0)


def load_noise16k(raw_path):
    """真机 8 kHz int16 底噪 → 16 kHz float，供 16k 域叠加。"""
    pcm = np.fromfile(os.path.expanduser(raw_path), dtype="<i2").astype(np.float32)
    if pcm.size == 0:
        raise ValueError("底噪文件为空: %s" % raw_path)
    return np.clip(resample_poly(pcm / 32768.0, 2, 1), -1.0, 1.0)


def add_noise(audio, bank16k, snr_db, rng):
    n = len(audio)
    if len(bank16k) < n:
        bank16k = np.tile(bank16k, int(np.ceil(n / len(bank16k))))
    off = int(rng.integers(0, max(1, len(bank16k) - n)))
    seg = bank16k[off:off + n]
    sig_p = float(np.mean(audio ** 2))
    noi_p = float(np.mean(seg ** 2))
    if noi_p <= 1e-12 or sig_p <= 1e-12:
        return audio
    scale = np.sqrt(sig_p / (10.0 ** (snr_db / 10.0)) / noi_p)
    return np.clip(audio + seg * scale, -1.0, 1.0).astype(np.float32)


def to_features(audio16k, rng):
    """16k float → (T,40) float32，走固件 C 链路 + 2 s 真机底噪预热。"""
    return df.features_from_audio16k(np.asarray(audio16k, dtype=np.float32), rng=rng)


def cut_windows(feats, win=WIN_FRAMES, hop=HOP_FRAMES):
    if feats is None or len(feats) < MIN_FRAMES:
        return []
    starts = range(0, max(1, len(feats) - win + 1), hop)
    return [np.asarray(feats[s:s + win], dtype=np.float32)
            for s in starts if len(feats[s:s + win]) >= MIN_FRAMES]


def speaker_groups(speakers, seed, val_ratio=0.15, test_ratio=0.15):
    """按说话人分组；测试集里的人不会出现在训练/验证里。"""
    s = sorted(set(speakers))
    rng = np.random.default_rng(seed)
    rng.shuffle(s)
    n_test = max(1, int(round(len(s) * test_ratio)))
    n_val = max(1, int(round(len(s) * val_ratio)))
    if len(s) - n_test - n_val < 1:
        raise ValueError("说话人太少，无法同时留出训练/验证/测试")
    return {
        "testing": s[:n_test],
        "validation": s[n_test:n_test + n_val],
        "training": s[n_test + n_val:],
    }


def write_split(out_root, split, windows):
    from mmap_ninja.ragged import RaggedMmap
    if not windows:
        return 0
    out_dir = os.path.join(out_root, split)
    os.makedirs(out_dir, exist_ok=True)
    target = os.path.join(out_dir, "wakeword_mmap")
    if os.path.exists(target):
        shutil.rmtree(target)
    RaggedMmap.from_generator(
        out_dir=target,
        sample_generator=(np.asarray(w, dtype=np.float32) for w in windows),
        batch_size=100,
        verbose=False,
    )
    return len(windows)


def write_manifest(path, data):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2, sort_keys=True)
    os.replace(tmp, path)


def find_ffmpeg():
    """按开发机资产清单 §2 的规矩找 ffmpeg。

    清单 §5 登记的是 ~/bin/ffmpeg（168 MB），而它不在**非交互 ssh 的 PATH** 里，
    所以不能用 which 单点判断（清单 §2 点名的历史坑）。
    顺序：FFMPEG_BINARY 环境变量 → ~/bin/ffmpeg → 交互 shell → 当前 PATH。
    """
    import shutil
    import subprocess
    candidates = []
    env = os.environ.get("FFMPEG_BINARY")
    if env:
        candidates.append(os.path.expanduser(env))
    candidates.append(os.path.expanduser("~/bin/ffmpeg"))
    here = shutil.which("ffmpeg")
    if here:
        candidates.append(here)
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    try:
        r = subprocess.run(["bash", "-lc", "command -v ffmpeg"],
                           capture_output=True, text=True, timeout=10)
        p = r.stdout.strip().splitlines()
        if r.returncode == 0 and p and os.path.isfile(p[0]):
            return p[0]
    except Exception:
        pass
    return None


def decode_audio_bytes(data, want_rate=16000):
    """TTS 返回 MP3：优先用清单登记的 ffmpeg，再退 soundfile(libsndfile>=1.1)。

    开发机上 imageio-ffmpeg / av 已按清单 §12.1 清理，不要依赖它们。
    """
    import subprocess
    errors = []
    exe = find_ffmpeg()
    if exe:
        p = subprocess.run(
            [exe, "-loglevel", "error", "-i", "pipe:0", "-f", "f32le",
             "-ac", "1", "-ar", str(want_rate), "pipe:1"],
            input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120,
        )
        if p.returncode == 0 and p.stdout:
            return np.frombuffer(p.stdout, dtype="<f4").copy()
        errors.append("ffmpeg rc=%d: %s" % (p.returncode, p.stderr[:200].decode("utf-8", "replace")))
    else:
        errors.append("未找到 ffmpeg（清单 §5 应为 ~/bin/ffmpeg）")
    try:
        import soundfile as sf
        x, sr = sf.read(io.BytesIO(data), dtype="float32", always_2d=False)
        if getattr(x, "ndim", 1) > 1:
            x = x.mean(axis=1)
        if sr != want_rate:
            g = np.gcd(int(sr), want_rate)
            x = resample_poly(x, want_rate // g, int(sr) // g)
        return np.clip(np.asarray(x, dtype=np.float32), -1.0, 1.0)
    except Exception as e:
        errors.append("soundfile: %s" % e)
    raise RuntimeError("无法解码 MP3：\n" + "\n".join(errors))
