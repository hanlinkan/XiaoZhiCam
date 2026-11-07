#!/usr/bin/env python3
"""按「固件判据」判卷：召回率 + 误报率，口径与设备一致。

为什么要单独有一个判卷器：
  microWakeWord 训练日志里的 `tflite_streaming_roc.txt` 用的是 **5 帧（150 ms）
  滑动平均**再取峰值（test.py 的 sliding_window_length 默认 5，训练流程没覆盖），
  而固件 kws.c 是 **单帧判据**：`if (prob >= threshold)`。两套口径下同一个模型
  的召回能差 10 个百分点以上 —— 拿训练日志当上线依据会误判。

所以这里把口径显式化：
  * --smooth 1  固件口径（默认），单帧峰值过门限即命中
  * --smooth 5  训练日志口径，与 tflite_streaming_roc.txt 对齐
  * --smooth 3  折中：固件若将来加去抖，多半用 3 帧

误报率按「每段音频最多记一次」统计（比 microWakeWord 的 ignore_slices 更保守），
时长 = 输出概率个数 × stride × window_step_ms。

用法（开发机）：
  ~/miniconda3/envs/mww/bin/python judge_firmware.py \
      --model <...>/stream_state_internal_quant.tflite \
      --pos ~/theplace/XiaoZhiCam/mww_data/features_v18_pos \
      --neg ~/theplace/XiaoZhiCam/mww_data/features_public_v2 \
      --neg ~/theplace/XiaoZhiCam/mww_data/multivoice_v2/features \
      --neg ~/theplace/XiaoZhiCam/mww_data/features_v17_noise \
      --split testing --limit 300 --threshold 240 \
      --report /tmp/verdict_v18.json
"""
import argparse
import json
import os
import sys

import numpy as np
from ai_edge_litert.interpreter import Interpreter, OpResolverType
from mmap_ninja.ragged import RaggedMmap


def make_interp(model_path):
    interp = Interpreter(model_path=model_path,
                         experimental_op_resolver_type=OpResolverType.BUILTIN_REF)
    interp.allocate_tensors()
    return interp, interp.get_input_details()[0], interp.get_output_details()[0]


def prob_track(factory, feats, max_frames, scale, zp):
    """(T,40) 特征 -> 概率序列（每 3 帧一个输出，对应固件攒帧节奏）。

    每条窗口新建 Interpreter：reset_all_variables() 清不掉这个流式模型的内部
    状态，复用会让样本之间的分数互相污染（历史踩过）。
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
    probs = []
    for i in range(0, len(q) - 2, 3):
        chunk = q[i:i + 3]
        if chunk.shape[0] < 3:
            break
        interp.set_tensor(inp["index"], chunk.reshape(inp["shape"]))
        interp.invoke()
        probs.append(int(interp.get_tensor(out["index"]).reshape(-1)[0]) & 0xFF)
    return probs


def smoothed_peak(probs, smooth):
    if not probs:
        return 0
    if smooth <= 1:
        return max(probs)
    arr = np.asarray(probs, dtype=np.float64)
    if arr.size < smooth:
        return int(arr.mean())
    win = np.lib.stride_tricks.sliding_window_view(arr, smooth)
    return int(round(float(win.mean(axis=-1).max())))


def run_dir(factory, root, split, limit, seed, smooth, args, scale, zp):
    target = os.path.join(root, split, "wakeword_mmap")
    if not os.path.isdir(target):
        return {"dir": root, "error": "缺少 %s" % target}
    mm = RaggedMmap(target)
    total = len(mm)
    n = min(total, limit) if limit else total
    if n < total:
        rng = np.random.default_rng(seed)
        idx = np.sort(rng.choice(total, size=n, replace=False))
    else:
        idx = np.arange(total)

    peaks, frames = [], 0
    for i in idx:
        probs = prob_track(factory, mm[int(i)], args.max_frames, scale, zp)
        frames += len(probs) * args.stride
        peaks.append(smoothed_peak(probs, smooth))

    arr = np.asarray(peaks, dtype=np.int64)
    hits = int((arr >= args.threshold).sum())
    secs = frames * args.window_step_ms / 1000.0
    return {"dir": os.path.abspath(root), "total": total, "scored": int(n),
            "hits": hits, "rate": round(hits / n, 4) if n else 0.0,
            "seconds": round(secs, 2),
            "per_hour": round(hits / (secs / 3600.0), 3) if secs > 0 else 0.0,
            "peaks": peaks}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", required=True)
    ap.add_argument("--pos", action="append", default=[], help="正样本特征根目录，可重复")
    ap.add_argument("--neg", action="append", default=[], help="负样本特征根目录，可重复")
    ap.add_argument("--split", default="testing")
    ap.add_argument("--limit", type=int, default=300, help="每目录最多评多少条，0=全部")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--threshold", type=int, default=240, help="固件命中门限")
    ap.add_argument("--smooth", type=int, default=1,
                    help="滑窗帧数：1=固件单帧，5=训练日志口径")
    ap.add_argument("--max-frames", type=int, default=204)
    ap.add_argument("--stride", type=int, default=3)
    ap.add_argument("--window-step-ms", type=float, default=10.0)
    ap.add_argument("--report", default="")
    args = ap.parse_args()

    if not args.pos and not args.neg:
        ap.error("至少要给一个 --pos 或 --neg")

    model_path = os.path.expanduser(args.model)

    def factory():
        return make_interp(model_path)

    _, inp, _ = factory()
    scale, zp = inp.get("quantization", (0.0, 0))
    print("模型 %s" % os.path.basename(model_path))
    print("口径：smooth=%d 帧，门限 %d，量化 scale=%s zp=%s"
          % (args.smooth, args.threshold, scale, zp))

    out = {"model": os.path.abspath(model_path), "split": args.split,
           "threshold": args.threshold, "smooth": args.smooth,
           "limit": args.limit, "positive": [], "negative": []}

    for label, dirs in (("positive", args.pos), ("negative", args.neg)):
        for d in dirs:
            r = run_dir(factory, os.path.expanduser(d), args.split, args.limit,
                        args.seed, args.smooth, args, scale, zp)
            out[label].append(r)
            name = os.path.basename(os.path.normpath(d))
            if "error" in r:
                print("FAIL %-8s %-28s %s" % (label, name, r["error"]), file=sys.stderr)
            elif label == "positive":
                print("正样本 %-28s n=%-4d 命中 %-4d 召回 %.1f%%"
                      % (name, r["scored"], r["hits"], r["rate"] * 100))
            else:
                print("负样本 %-28s n=%-4d 命中 %-4d %.2f h -> %.2f 次/小时"
                      % (name, r["scored"], r["hits"], r["seconds"] / 3600.0,
                         r["per_hour"]))

    pos = [r for r in out["positive"] if "error" not in r]
    neg = [r for r in out["negative"] if "error" not in r]
    if pos:
        tot = sum(r["scored"] for r in pos)
        hit = sum(r["hits"] for r in pos)
        out["recall"] = round(hit / tot, 4) if tot else 0.0
        print("\n总召回 %d/%d = %.1f%%" % (hit, tot, out["recall"] * 100))
    if neg:
        secs = sum(r["seconds"] for r in neg)
        hits = sum(r["hits"] for r in neg)
        out["false_positives"] = hits
        out["negative_hours"] = round(secs / 3600.0, 3)
        out["false_positives_per_hour"] = round(hits / (secs / 3600.0), 3) if secs else 0.0
        print("总误报 %d 次 / %.2f 小时 = %.2f 次/小时"
              % (hits, secs / 3600.0, out["false_positives_per_hour"]))
        if hits == 0 and secs:
            print("（零误报时 95%% 置信上界约 %.2f 次/小时，三倍法则）"
                  % (3.0 / (secs / 3600.0)))

    if args.report:
        json.dump(out, open(os.path.expanduser(args.report), "w", encoding="utf-8"),
                  ensure_ascii=False, indent=2)
        print("\n报告已写出 %s" % args.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
