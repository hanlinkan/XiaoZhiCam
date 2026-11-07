#!/usr/bin/env python3
"""用现役 tflite 流式模型跑一遍特征窗口，验证「训练特征与设备判据对齐」。

重训前最容易白费功夫的坑：新管线生成的特征窗口，唤醒词落点和训练时不一致
（truncate_start 语义错了 / 切窗方向反了），模型怎么练都学不会。
这里直接拿**现役模型**给新特征打分：新生成的正样本应该普遍高分，
负样本应该普遍低分。分数不对就先修特征生成，别开始训练。

同时产出旧模型在新考卷上的基线（用于 eval_retrain.py 的回归对比）。

用法（开发机）：
  python check_feature_alignment.py \
      --model ~/theplace/XiaoZhiCam/mww_data/trained_models/wakeword_v16/.../stream_state_internal_quant.tflite \
      --positive ~/theplace/XiaoZhiCam/mww_data/features_v17_pos \
      --negative ~/theplace/XiaoZhiCam/mww_data/features_public_v1 \
      --negative ~/theplace/XiaoZhiCam/mww_data/features_v17_noise \
      --split testing --limit 40 --threshold 240 --report /tmp/align.json

只依赖 ai_edge_litert + mmap_ninja，不改模型、不训练。
"""
import argparse
import json
import os
import sys

import numpy as np
from ai_edge_litert.interpreter import Interpreter, OpResolverType
from mmap_ninja.ragged import RaggedMmap

HERE = os.path.dirname(os.path.abspath(__file__))


def score_window(interp, inp, out, win, max_frames, scale, zero_point):
    """把一条 (T,40) float32 特征按流式方式逐 3 帧喂进去，返回最高概率。"""
    x = np.asarray(win, dtype=np.float32)[-max_frames:]
    if x.ndim != 2 or x.shape[1] != 40:
        raise ValueError("特征形状应为 (T,40)，实际 %s" % (x.shape,))
    if scale:
        q = np.clip(np.round(x / scale + zero_point), -128, 127).astype(np.int8)
    else:
        q = x.astype(np.int8)
    try:
        interp.reset_all_variables()
    except Exception:
        pass
    peak = 0
    for i in range(0, len(q) - 2, 3):
        chunk = q[i:i + 3]
        if chunk.shape[0] < 3:
            break
        interp.set_tensor(inp["index"], chunk.reshape(inp["shape"]))
        interp.invoke()
        v = int(interp.get_tensor(out["index"]).reshape(-1)[0]) & 0xFF
        peak = max(peak, v)
    return peak


def run_dir(interp, inp, out, path, split, limit, args, scale, zp):
    target = os.path.join(path, split, "wakeword_mmap")
    if not os.path.isdir(target):
        return {"dir": path, "error": "缺少 %s" % target}
    try:
        mm = RaggedMmap(target)
    except Exception as e:
        return {"dir": path, "error": str(e)}
    n = min(len(mm), limit) if limit else len(mm)
    peaks = []
    for i in range(n):
        try:
            peaks.append(score_window(interp, inp, out, mm[i], args.max_frames, scale, zp))
        except Exception as e:
            return {"dir": path, "error": "第 %d 条推理失败: %s" % (i, e)}
    arr = np.asarray(peaks, dtype=np.float64)
    return {
        "dir": os.path.abspath(path),
        "total": len(mm),
        "scored": int(n),
        "max": int(arr.max()) if n else 0,
        "mean": round(float(arr.mean()), 1) if n else 0.0,
        "hits": int((arr >= args.threshold).sum()),
        "hit_rate": round(float((arr >= args.threshold).mean()), 3) if n else 0.0,
        "peaks": peaks,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", required=True, help="int8 流式 tflite 模型")
    ap.add_argument("--positive", action="append", default=[], help="正样本特征根目录，可重复")
    ap.add_argument("--negative", action="append", default=[], help="负样本特征根目录，可重复")
    ap.add_argument("--split", default="testing")
    ap.add_argument("--limit", type=int, default=40, help="每个目录最多评多少窗口，0=全部")
    ap.add_argument("--threshold", type=int, default=240, help="命中门限（与固件一致）")
    ap.add_argument("--max-frames", type=int, default=204,
                    help="取窗口末尾多少帧（对应训练配置 spectrogram_length）")
    ap.add_argument("--report", default="", help="JSON 报告输出路径")
    args = ap.parse_args()
    if not args.positive and not args.negative:
        ap.error("至少要给一个 --positive 或 --negative")

    interp = Interpreter(model_path=os.path.expanduser(args.model),
                         experimental_op_resolver_type=OpResolverType.BUILTIN_REF)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]
    scale, zp = inp.get("quantization", (0.0, 0))
    print("模型输入 %s %s %s  量化 scale=%s zp=%s" %
          (inp["name"], inp["shape"], inp["dtype"], scale, zp))

    report = {"model": os.path.abspath(os.path.expanduser(args.model)),
              "split": args.split, "threshold": args.threshold,
              "max_frames": args.max_frames, "positive": [], "negative": []}
    for label, dirs in (("positive", args.positive), ("negative", args.negative)):
        for d in dirs:
            r = run_dir(interp, inp, out, os.path.expanduser(d), args.split,
                        args.limit, args, scale, zp)
            report[label].append(r)
            if "error" in r:
                print("FAIL %-9s %s: %s" % (label, d, r["error"]), file=sys.stderr)
            else:
                print("%-9s %-46s n=%-4d max=%-3d mean=%-6.1f hit=%d/%d (%.0f%%)" %
                      (label, os.path.basename(d), r["scored"], r["max"], r["mean"],
                       r["hits"], r["scored"], r["hit_rate"] * 100))

    if args.report:
        with open(os.path.expanduser(args.report), "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
