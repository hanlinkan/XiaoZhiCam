#!/usr/bin/env python3
"""固定考卷的判卷器：把设备语义评测结果对照量化门槛，决定能不能上机。

分工：设备语义命中由 kws_host 产出（模型切换靠重新编译 mww_model.h），
本脚本只负责「判卷」——避免把门槛散落在训练日志里靠人眼看。

输入 results.json（由外部评测器生成）：
{
  "model": "v17",
  "positive": [{"id": "p001", "group": "Xiaoxiao", "hit": true}, ...],
  "negative": [{"id": "n001", "group": "S0724", "fp": 0, "seconds": 300}, ...],
  "cpu_percent": 12.4
}

判定：
  * 总召回 / 按音色或说话人分组的召回
  * 误报率 = 总误报 / 负样本总时长（次/小时）
  * 负样本时长不足时直接判失败（三倍法则：0 次 FP 的 95% 上界约 3/T）
  * 与 baseline 比较：旧版能过的正样本新版一条都不许丢
  * CPU 占比（若提供）

公开仓库不含模型与数据，CI 用 --self-test 验证判定逻辑本身。

用法：
  python eval_retrain.py --results out/v17.json --baseline out/v16.json \
      --thresholds src/kws/test/eval_thresholds.json --report out/verdict.json
  python eval_retrain.py --self-test
"""
import argparse
import json
import os
import sys

DEFAULT_THRESHOLDS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "eval_thresholds.json")


def load_json(path):
    with open(os.path.expanduser(path), "r", encoding="utf-8") as f:
        return json.load(f)


def evaluate(results, thresholds, baseline=None):
    problems = []
    pos = results.get("positive", [])
    neg = results.get("negative", [])
    if not pos:
        problems.append("考卷里没有正样本")
    if not neg:
        problems.append("考卷里没有负样本")

    hits = sum(1 for p in pos if p.get("hit"))
    recall = hits / len(pos) if pos else 0.0
    min_recall = float(thresholds.get("min_recall", 0.90))
    if recall < min_recall:
        problems.append("召回 %.3f < 门槛 %.2f" % (recall, min_recall))

    groups = {}
    for p in pos:
        g = p.get("group", "unknown")
        d = groups.setdefault(g, [0, 0])
        d[1] += 1
        d[0] += 1 if p.get("hit") else 0
    by_group = {g: round(h / n, 3) if n else 0.0 for g, (h, n) in groups.items()}
    weak = [g for g, v in by_group.items() if v < min_recall]
    if weak:
        problems.append("分组召回低于门槛: %s" % weak[:5])

    neg_seconds = sum(float(n.get("seconds", 0)) for n in neg)
    fps = sum(int(n.get("fp", 0)) for n in neg)
    hours = neg_seconds / 3600.0
    fp_rate = fps / hours if hours > 0 else float("inf")
    min_hours = float(thresholds.get("min_negative_hours", 6.0))
    if hours < min_hours:
        problems.append("负样本仅 %.2f h < 门槛 %.1f h，0 次误报的 95%% 上界约 %.1f 次/h" %
                        (hours, min_hours, 3.0 / hours if hours else float("inf")))
    max_fp = float(thresholds.get("max_fp_per_hour", 0.5))
    if fp_rate > max_fp:
        problems.append("误报率 %.2f 次/h > 门槛 %.2f" % (fp_rate, max_fp))
    upper_95 = 3.0 / hours if hours else None

    regression = []
    if baseline and thresholds.get("require_no_regression", True):
        old_ok = {p["id"] for p in baseline.get("positive", []) if p.get("hit")}
        now_ok = {p["id"] for p in pos if p.get("hit")}
        regression = sorted(old_ok - now_ok)
        if regression:
            problems.append("回归：旧版能过、新版丢失 %d 条（%s）" %
                            (len(regression), regression[:5]))

    cpu = results.get("cpu_percent")
    max_cpu = float(thresholds.get("max_cpu_percent", 15.0))
    if cpu is not None and float(cpu) > max_cpu:
        problems.append("单核开销 %.1f%% > 门槛 %.1f%%" % (float(cpu), max_cpu))

    return {
        "model": results.get("model", "unknown"),
        "recall": round(recall, 4),
        "recall_by_group": by_group,
        "positive_total": len(pos), "positive_hits": hits,
        "negative_total": len(neg), "negative_hours": round(hours, 3),
        "false_positives": fps,
        "fp_per_hour": round(fp_rate, 4) if hours else None,
        "fp_upper_95": round(upper_95, 4) if upper_95 else None,
        "cpu_percent": cpu,
        "regression_ids": regression,
        "thresholds": thresholds,
        "passed": not problems,
        "problems": problems,
    }


def self_test():
    th = load_json(DEFAULT_THRESHOLDS)
    pos = [{"id": "p%02d" % i, "group": g, "hit": True}
           for i, g in enumerate(["A"] * 10 + ["B"] * 10)]
    neg = [{"id": "n%02d" % i, "group": "S%d" % (i % 20), "fp": 0, "seconds": 1200}
           for i in range(20)]  # 20 * 1200 s = 6.67 h
    good = {"model": "fake-v17", "positive": pos, "negative": neg, "cpu_percent": 12.0}
    r = evaluate(good, th, baseline=good)
    assert r["passed"], "达标样例应通过: %s" % r["problems"]
    assert r["negative_hours"] >= th["min_negative_hours"]
    print("[self-test] 达标样例通过，召回 %.2f，负样本 %.2f h" %
          (r["recall"], r["negative_hours"]))

    cases = []
    bad_recall = json.loads(json.dumps(good))
    bad_recall["positive"] = [dict(p, hit=False) for p in pos]
    cases.append(("召回不足", bad_recall))

    bad_fp = json.loads(json.dumps(good))
    bad_fp["negative"][0]["fp"] = 30
    cases.append(("误报超标", bad_fp))

    short = json.loads(json.dumps(good))
    short["negative"] = neg[:2]
    cases.append(("负样本时长不足", short))

    bad_reg = json.loads(json.dumps(good))
    bad_reg["positive"][0]["hit"] = False
    cases.append(("回归丢失", bad_reg))

    for name, data in cases:
        # 回归用例需要 baseline 里该样本是 hit
        base = good if name == "回归丢失" else None
        r2 = evaluate(data, th, baseline=base)
        assert not r2["passed"], "%s 应当被拦截" % name
        print("[self-test] %s 被拦截: %s" % (name, r2["problems"][0][:56]))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", help="设备语义评测结果 JSON")
    ap.add_argument("--baseline", help="旧模型结果 JSON，用于回归检查")
    ap.add_argument("--thresholds", default=DEFAULT_THRESHOLDS)
    ap.add_argument("--report", default="", help="判卷报告输出路径")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.results:
        ap.error("需要 --results（或用 --self-test）")

    th = load_json(args.thresholds)
    base = load_json(args.baseline) if args.baseline else None
    verdict = evaluate(load_json(args.results), th, base)
    if args.report:
        with open(os.path.expanduser(args.report), "w", encoding="utf-8") as f:
            json.dump(verdict, f, ensure_ascii=False, indent=2)

    print("模型 %s  召回 %.3f  误报 %s 次/h  负样本 %.2f h" %
          (verdict["model"], verdict["recall"],
           verdict["fp_per_hour"], verdict["negative_hours"]))
    print("分组召回: %s" % verdict["recall_by_group"])
    if verdict["fp_upper_95"]:
        print("0 次误报时 95%% 置信上界 = %.2f 次/h" % verdict["fp_upper_95"])
    if verdict["problems"]:
        for p in verdict["problems"]:
            print("FAIL: " + p, file=sys.stderr)
        return 1
    print("OK: 通过全部量化门槛，可进入真机 A/B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
