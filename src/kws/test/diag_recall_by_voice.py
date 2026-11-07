#!/usr/bin/env python3
"""按音色拆解召回：定位「漏检到底漏在谁身上」。

判卷只给一个总召回，训练却需要知道短板在哪。这个脚本对合成语料里的
**原始 WAV**（不带噪声/混响增强）逐条走部署链路 —— 8 kHz 降采样 + 固件 C
特征（libkwsfeat.so）+ int8 流式 tflite —— 再按音色 / 文本 / 韵律汇总。

和 check_feature_alignment.py 的分工：
  * 那个脚本评「特征目录」（mmap，已增强、已切窗），用来看整批分布；
  * 这个脚本评「原始音频」，能把分数对回音色，用来看泛化短板。

用法（开发机）：
  ~/miniconda3/envs/mww/bin/python diag_recall_by_voice.py \
      --model ~/theplace/XiaoZhiCam/mww_data/trained_models/wakeword_v18/\
tflite_stream_state_internal_quant/stream_state_internal_quant.tflite \
      --corpus ~/theplace/XiaoZhiCam/mww_data/multivoice_v2 \
      --split heldout --threshold 240 --report /tmp/v18_by_voice.json

只依赖 ai_edge_litert + numpy/scipy，不改模型、不训练。
"""
import argparse
import collections
import json
import os
import sys
import wave

import numpy as np
from ai_edge_litert.interpreter import Interpreter, OpResolverType
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

MWW_DATA = os.path.expanduser("~/theplace/XiaoZhiCam/mww_data")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")


def load_wav16k(path):
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise SystemExit("只支持 16bit 单声道：%s" % path)
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32)


def to8k(x, rate=16000):
    if rate == 8000:
        return x.astype(np.int16)
    if rate != 16000:
        raise SystemExit("不支持的采样率 %d" % rate)
    y = resample_poly(x, 1, 2)
    return np.clip(np.round(y), -32768, 32767).astype(np.int16)


def make_interp(model_path):
    interp = Interpreter(model_path=model_path,
                         experimental_op_resolver_type=OpResolverType.BUILTIN_REF)
    interp.allocate_tensors()
    return interp, interp.get_input_details()[0], interp.get_output_details()[0]


def score_features(factory, feats, max_frames, scale, zp, ignore_after):
    """按流式语义逐 3 帧喂入，(T,40) 特征 -> 峰值概率。

    每条都用全新 Interpreter：reset_all_variables() 清不干净这个流式模型的
    内部状态，复用会让不同样本之间的分数互相污染。
    """
    x = np.asarray(feats, dtype=np.float32)
    if x.ndim != 2 or x.shape[1] != 40:
        raise ValueError("特征形状应为 (T,40)，实际 %s" % (x.shape,))
    if max_frames and x.shape[0] > max_frames:
        x = x[-max_frames:]
    if scale:
        q = np.clip(np.round(x / scale + zp), -128, 127).astype(np.int8)
    else:
        q = x.astype(np.int8)
    interp, inp, out = factory()
    n = len(q)
    start = 0
    if ignore_after and n > ignore_after:
        start = ignore_after
    peak = 0
    for i in range(start, n - 2, 3):
        chunk = q[i:i + 3]
        if chunk.shape[0] < 3:
            break
        interp.set_tensor(inp["index"], chunk.reshape(inp["shape"]))
        interp.invoke()
        v = int(interp.get_tensor(out["index"]).reshape(-1)[0]) & 0xFF
        if v > peak:
            peak = v
    return peak


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", required=True, help="int8 流式 tflite 模型")
    ap.add_argument("--corpus", required=True, help="含 manifest.json 的合成语料根目录")
    ap.add_argument("--split", default="heldout",
                    help="manifest 里的 split 字段（train/val/heldout，逗号分隔可多选）")
    ap.add_argument("--threshold", type=int, default=240, help="命中门限（与固件一致）")
    ap.add_argument("--max-frames", type=int, default=204,
                    help="只取窗口末尾多少帧（对应 spectrogram_length）")
    ap.add_argument("--ignore-after", type=int, default=0,
                    help="前多少帧不参与打分（对应流式评测的 ignore_slices_after_accept）")
    ap.add_argument("--noise", default=os.path.join(MWW_DATA, "noise30m.raw"),
                    help="8 kHz int16 真机底噪，用于前级预热")
    ap.add_argument("--report", default="", help="JSON 报告输出路径")
    args = ap.parse_args()

    corpus = os.path.expanduser(args.corpus)
    manifest = json.load(open(os.path.join(corpus, "manifest.json"), encoding="utf-8"))
    wanted = {s.strip() for s in args.split.split(",") if s.strip()}
    rows = [f for f in manifest["files"]
            if f.get("label") == 1 and f.get("split") in wanted]
    if not rows:
        raise SystemExit("manifest 里没有 split=%s 的正样本" % args.split)

    # 部署链路：把前级自适应状态预热，否则冷启动窗口和连续流不同分布。
    import mww_deploy_features as df
    if os.path.exists(args.noise):
        df.NOISE_RAW = args.noise
    else:
        print("警告：找不到底噪 %s，特征将不带预热（分数会偏低）" % args.noise,
              file=sys.stderr)

    def factory():
        return make_interp(args.model)

    _, inp, _ = factory()
    scale, zp = inp.get("quantization", (0.0, 0))
    print("模型 %s" % os.path.basename(args.model))
    print("量化 scale=%s zp=%s；判 %d 条（split=%s），门限 %d"
          % (scale, zp, len(rows), args.split, args.threshold))

    detail = []
    for r in rows:
        wav = os.path.join(corpus, r["path"])
        pcm8k = to8k(load_wav16k(wav))
        feats = df.features_from_pcm8k(pcm8k)
        peak = score_features(factory, feats, args.max_frames, scale, zp,
                              args.ignore_after)
        detail.append({
            "path": r["path"], "voice": r["voice"], "text": r.get("text", ""),
            "rate": r.get("rate", ""), "pitch": r.get("pitch", ""),
            "volume": r.get("volume", ""), "peak": peak,
            "hit": bool(peak >= args.threshold),
        })

    def agg(key):
        g = collections.defaultdict(lambda: [0, 0])
        for d in detail:
            g[d[key]][0] += 1 if d["hit"] else 0
            g[d[key]][1] += 1
        return {k: {"hit": v[0], "n": v[1], "rate": round(v[0] / v[1], 3)}
                for k, v in sorted(g.items())}

    by_voice, by_text = agg("voice"), agg("text")
    hits = sum(1 for d in detail if d["hit"])
    print("\n== 按音色 ==")
    for k, v in by_voice.items():
        mark = "" if v["rate"] >= 0.9 else "   <-- 短板"
        print("  %-34s %2d/%2d  %.0f%%%s" % (k, v["hit"], v["n"], v["rate"] * 100, mark))
    print("\n== 按文本 ==")
    for k, v in by_text.items():
        print("  %-18s %2d/%2d  %.0f%%" % (k, v["hit"], v["n"], v["rate"] * 100))
    print("\n总召回 %d/%d = %.1f%%" % (hits, len(detail), 100.0 * hits / len(detail)))

    missed = [d for d in detail if not d["hit"]]
    if missed:
        print("\n== 漏检明细（%d 条）==" % len(missed))
        for d in sorted(missed, key=lambda x: x["peak"]):
            print("  peak=%-3d %-34s %-6s %-7s %-6s %s"
                  % (d["peak"], d["voice"], d["rate"], d["pitch"], d["volume"], d["text"]))

    if args.report:
        json.dump({"model": os.path.abspath(os.path.expanduser(args.model)),
                   "split": args.split, "threshold": args.threshold,
                   "total": len(detail), "hits": hits,
                   "recall": round(hits / len(detail), 4),
                   "by_voice": by_voice, "by_text": by_text,
                   "detail": detail},
                  open(os.path.expanduser(args.report), "w", encoding="utf-8"),
                  ensure_ascii=False, indent=2)
        print("\n报告已写出 %s" % args.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
