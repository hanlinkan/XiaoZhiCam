#!/usr/bin/env python3
"""训练前数据断言：确认「反面教材」真的会进训练。

历史教训（docs/07 补记 §3）：「没有任何一处日志能证明负样本真的进入了训练」，
验证集里几乎没有负样本（faph=nan），结果 v7 出现「负样本越多、误报越多」。
在继续调参之前，必须先能回答：正负各有多少、每个 batch 大概抽到多少负样本。

检查项：
  1. 配置里每个 features_dir 是否存在、training split 是否真的有数据
  2. 正 / 负样本条数与 sampling_weight
  3. 按权重模拟 batch 抽样，估算负样本占比（过高过低都失败）
  4. validation / testing 合计必须有负样本，否则 FAP 全是 nan
  5. 可选：manifest 里的留出音色 / 说话人与训练零交集

不改第三方 microWakeWord 代码；独立在训练启动前跑，失败时 exit 1。
公开仓库不含训练数据，CI 用 --self-test（临时合成数据）。

用法：
  python verify_training_data.py --config training_parameters_v16.yaml \
      --data-root <features 根目录> --report /tmp/data_check.json
  python verify_training_data.py --self-test
"""
import argparse
import json
import os
import sys
import tempfile

SPLITS = ("training", "validation", "testing")


def count_split(feat_dir, split):
    """返回该 split 的样本数；缺目录/读不了都算 0，不抛异常。"""
    target = os.path.join(feat_dir, split, "wakeword_mmap")
    if not os.path.isdir(target):
        return 0
    try:
        from mmap_ninja.ragged import RaggedMmap
        return len(RaggedMmap(target))
    except Exception:
        return 0


def resolve_dir(raw, data_root):
    p = os.path.expanduser(raw)
    if os.path.isabs(p):
        return p
    p = p.lstrip("./")
    if p.startswith("data/"):
        p = p[len("data/"):]
    return os.path.join(os.path.expanduser(data_root), p)


def load_entries(config_path, data_root):
    import yaml
    with open(os.path.expanduser(config_path), "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f) or {}
    out = []
    for e in cfg.get("features", []):
        if not isinstance(e, dict) or "features_dir" not in e:
            continue
        d = resolve_dir(e["features_dir"], data_root)
        out.append({
            "dir": d,
            "raw": e["features_dir"],
            "exists": os.path.isdir(d),
            "truth": bool(e.get("truth", False)),
            "weight": float(e.get("sampling_weight", 1.0)),
            "counts": {s: count_split(d, s) for s in SPLITS},
        })
    return out


def check(entries, args, manifests=None):
    problems = []
    active = [e for e in entries if e["weight"] > 0]
    if not active:
        problems.append("配置里没有任何 sampling_weight > 0 的特征目录")

    pos = [e for e in active if e["truth"]]
    neg = [e for e in active if not e["truth"]]
    if not pos or sum(e["counts"]["training"] for e in pos) == 0:
        problems.append("没有可用的正样本 training 数据")
    if not neg or sum(e["counts"]["training"] for e in neg) == 0:
        problems.append("没有可用的负样本 training 数据 —— 模型只会学到『一律唤醒』")

    missing = [e["raw"] for e in active if not e["exists"]]
    if missing:
        problems.append("特征目录不存在（检查 --data-root 是否指对）: %s" % missing[:8])
    for e in active:
        if e["exists"] and e["counts"]["training"] == 0:
            problems.append("目录存在但 training 为空（会被训练静默跳过）: %s" % e["raw"])

    neg_val = sum(e["counts"]["validation"] for e in neg)
    neg_test = sum(e["counts"]["testing"] for e in neg)
    if neg_val == 0:
        problems.append("validation 里没有负样本，训练日志的 precision/FAP 会是 nan")
    if neg_test == 0:
        problems.append("testing 里没有负样本，FAP 选模指标不可信")

    # 按 sampling_weight 模拟抽样，估算每个 batch 的负样本占比
    import numpy as np
    weights = np.array([e["weight"] for e in active], dtype=np.float64)
    total_w = weights.sum()
    neg_ratio = None
    if total_w > 0 and len(active) > 1:
        rng = np.random.default_rng(args.seed)
        probs = weights / total_w
        n = args.batches * args.batch_size
        picks = rng.choice(len(active), size=n, p=probs)
        neg_ratio = float(np.mean([not active[i]["truth"] for i in picks]))
        if not (args.min_neg_ratio <= neg_ratio <= args.max_neg_ratio):
            problems.append("模拟 batch 负样本占比 %.3f 超出 [%.2f, %.2f]" %
                            (neg_ratio, args.min_neg_ratio, args.max_neg_ratio))
    elif total_w > 0:
        problems.append("只有一个有效特征目录，无法验证正负混合")

    # 留出音色 / 说话人零交集
    overlap = []
    if manifests:
        held = set()
        train = set()
        for mp in manifests:
            with open(os.path.expanduser(mp), "r", encoding="utf-8") as f:
                m = json.load(f)
            held.update(m.get("heldout_voices", []))
            held.update(m.get("split_speakers", {}).get("testing", []))
            train.update(m.get("split_speakers", {}).get("training", []))
            for rec in m.get("files", []):
                if rec.get("split") == "train":
                    train.add(rec.get("voice", ""))
        overlap = sorted(held & train)
        if overlap:
            problems.append("考卷与训练存在音色/说话人交集: %s" % overlap[:5])

    report = {
        "config": os.path.abspath(os.path.expanduser(args.config)) if args.config else None,
        "data_root": os.path.abspath(os.path.expanduser(args.data_root)),
        "entries": [{"dir": e["raw"], "truth": e["truth"], "weight": e["weight"],
                     "exists": e["exists"], "counts": e["counts"]} for e in entries],
        "missing_dirs": [e["raw"] for e in active if not e["exists"]],
        "pos_training": sum(e["counts"]["training"] for e in pos),
        "neg_training": sum(e["counts"]["training"] for e in neg),
        "neg_validation": neg_val,
        "neg_testing": neg_test,
        "simulated_neg_ratio": neg_ratio,
        "holdout_overlap": overlap,
        "problems": problems,
    }
    return report


def self_test(args):
    """临时造一份小型正负特征 + 配置，验证断言逻辑本身（CI 用）。"""
    from mmap_ninja.ragged import RaggedMmap
    import numpy as np
    import yaml

    tmp = tempfile.mkdtemp(prefix="verify_kws_")
    def make(name, truth, n=6):
        root = os.path.join(tmp, name)
        for s in SPLITS:
            out = os.path.join(root, s)
            os.makedirs(out, exist_ok=True)
            RaggedMmap.from_generator(
                out_dir=os.path.join(out, "wakeword_mmap"),
                sample_generator=(np.zeros((150, 40), dtype=np.float32) for _ in range(n)),
                batch_size=10, verbose=False)
        return {"features_dir": "./data/" + name, "sampling_weight": 2.0 if truth else 8.0,
                "truth": truth, "type": "mmap"}

    cfg = {"features": [make("pos", True), make("neg", False)]}
    cfg_path = os.path.join(tmp, "cfg.yaml")
    with open(cfg_path, "w", encoding="utf-8") as f:
        yaml.safe_dump(cfg, f)

    class A:
        seed, batches, batch_size = 1234, 20, 128
        min_neg_ratio, max_neg_ratio = 0.2, 0.95
        config, data_root = cfg_path, tmp

    r = check(load_entries(cfg_path, tmp), A(), None)
    assert not r["problems"], "自检应通过，实际: %s" % r["problems"]
    assert r["pos_training"] > 0 and r["neg_training"] > 0
    assert r["simulated_neg_ratio"] is not None
    print("[self-test] 正常配置通过；模拟负样本占比 %.3f" % r["simulated_neg_ratio"])

    # 反例：把负样本权重设为 0，必须被抓出来
    cfg["features"][1]["sampling_weight"] = 0.0
    with open(cfg_path, "w", encoding="utf-8") as f:
        yaml.safe_dump(cfg, f)
    r2 = check(load_entries(cfg_path, tmp), A(), None)
    assert r2["problems"], "反例（无负样本）应当失败"
    print("[self-test] 反例被正确拦截: %s" % r2["problems"][0][:60])
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", help="训练配置 YAML")
    ap.add_argument("--data-root", default="./mww_data", help="相对 ./data/ 的根目录")
    ap.add_argument("--manifest", default="", help="逗号分隔的 manifest.json")
    ap.add_argument("--batches", type=int, default=100)
    ap.add_argument("--batch-size", type=int, default=128)
    ap.add_argument("--min-neg-ratio", type=float, default=0.2)
    ap.add_argument("--max-neg-ratio", type=float, default=0.95)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--report", default="", help="JSON 报告输出路径")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test(args)
    if not args.config:
        ap.error("需要 --config（或用 --self-test）")

    manifests = [m for m in args.manifest.split(",") if m.strip()]
    report = check(load_entries(args.config, args.data_root), args, manifests)
    if args.report:
        with open(os.path.expanduser(args.report), "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)

    print("正样本 training=%d  负样本 training=%d / validation=%d / testing=%d" %
          (report["pos_training"], report["neg_training"],
           report["neg_validation"], report["neg_testing"]))
    if report["simulated_neg_ratio"] is not None:
        print("模拟 batch 负样本占比 = %.3f" % report["simulated_neg_ratio"])
    if report["problems"]:
        for p in report["problems"]:
            print("FAIL: " + p, file=sys.stderr)
        return 1
    print("OK: 训练数据断言通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
