#!/usr/bin/env python3
"""把 gen_multivoice.py 产出的正样本 WAV 变成按「音色」分组的训练特征。

为什么需要这一层：
  * 正样本 WAV 散落在 positive/<voice>/ 下，microWakeWord 的 Clips 只吃扁平目录；
  * gen_features_deploy.py 按**文件**随机切 train/val/test，同一个音色会同时
    出现在训练和考卷里，等于自己考自己；
  * 因此这里按 manifest 的 split 分成「训练音色」与「留出音色」两组，
    分别调 gen_features_deploy.py，再用留出音色的 testing 组装考卷。

输出是一个标准特征目录（training / validation / testing 各有 wakeword_mmap），
可直接写进训练配置的 features 列表（truth: true, truncate_start）。

用法：
  python prepare_positive_features.py \
      --corpus ~/theplace/XiaoZhiCam/mww_data/multivoice_v1 \
      --out    ~/theplace/XiaoZhiCam/mww_data/features_v17_pos --repeat 4
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "gen_features_deploy.py")
DEFAULT_MWW = "~/theplace/XiaoZhiCam/mww_data"


def voice_tag(name):
    return name.replace("zh-CN-", "").replace("-", "_")


def count_split(root, split):
    from mmap_ninja.ragged import RaggedMmap
    p = os.path.join(root, split, "wakeword_mmap")
    return len(RaggedMmap(p)) if os.path.isdir(p) else 0


def flatten(corpus, records, dest):
    os.makedirs(dest, exist_ok=True)
    for f in os.listdir(dest):
        os.remove(os.path.join(dest, f))
    for rec in records:
        src = os.path.abspath(os.path.join(corpus, rec["path"]))
        if not os.path.isfile(src):
            raise FileNotFoundError("正样本 WAV 不存在: %s" % src)
        name = voice_tag(rec["voice"]) + "__" + os.path.basename(rec["path"])
        os.symlink(src, os.path.join(dest, name))
    return dest


def run_gen(py, src, out, repeat, noise, all_testing=False):
    cmd = [py, GEN, "--src", src, "--out", out,
           "--repeat", str(repeat), "--noise", noise, "--overwrite"]
    if all_testing:
        cmd.append("--all-testing")
    print("$ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def place(src_root, split, out_root):
    s = os.path.join(src_root, split, "wakeword_mmap")
    if not os.path.isdir(s):
        raise RuntimeError("缺少特征目录: %s" % s)
    d = os.path.join(out_root, split, "wakeword_mmap")
    os.makedirs(os.path.dirname(d), exist_ok=True)
    if os.path.isdir(d):
        shutil.rmtree(d)
    elif os.path.lexists(d):
        os.remove(d)
    shutil.copytree(s, d)
    return count_split(out_root, split)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", required=True, help="gen_multivoice.py 的输出根目录")
    ap.add_argument("--out", required=True, help="输出特征目录")
    ap.add_argument("--repeat", type=int, default=4, help="训练音色的增强轮数")
    ap.add_argument("--holdout-repeat", type=int, default=20,
                    help="留出音色的增强轮数（每条原始音频重复增强，撑起考卷样本量）")
    ap.add_argument("--noise", default=DEFAULT_MWW + "/noise30m.raw")
    ap.add_argument("--python", default=sys.executable)
    args = ap.parse_args()

    corpus = os.path.expanduser(args.corpus)
    out_root = os.path.expanduser(args.out)
    mp = os.path.join(corpus, "manifest.json")
    with open(mp, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    pos = [r for r in manifest["files"] if r["label"] == 1]
    train = [r for r in pos if r.get("split") == "train"]
    heldout = [r for r in pos if r.get("split") == "heldout"]
    if not train or not heldout:
        raise SystemExit("manifest 里缺少 train/heldout 正样本；请带 --holdout-voices 重新生成")

    train_voices = sorted({r["voice"] for r in train})
    held_voices = sorted({r["voice"] for r in heldout})
    overlap = sorted(set(train_voices) & set(held_voices))
    if overlap:
        raise SystemExit("训练音色与考卷音色有交集: %s" % overlap)
    declared = set(manifest.get("heldout_voices", []))
    if declared and not set(held_voices).issubset(declared):
        raise SystemExit("留出音色与 manifest.heldout_voices 不一致")

    work = os.path.join(out_root, "_work")
    os.makedirs(work, exist_ok=True)
    src_train = flatten(corpus, train, os.path.join(work, "wav_train"))
    src_held = flatten(corpus, heldout, os.path.join(work, "wav_heldout"))

    gen_train = os.path.join(work, "gen_train")
    gen_held = os.path.join(work, "gen_heldout")
    run_gen(args.python, src_train, gen_train, args.repeat, args.noise)
    run_gen(args.python, src_held, gen_held, args.holdout_repeat, args.noise,
            all_testing=True)

    counts = {
        "training": place(gen_train, "training", out_root),
        "validation": place(gen_train, "validation", out_root),
        "testing": place(gen_held, "testing", out_root),
    }
    if any(n == 0 for n in counts.values()):
        raise SystemExit("有 split 为空: %s" % counts)
    print("特征组装完成: %s" % counts)

    info = {
        "corpus": os.path.abspath(corpus),
        "generator": "gen_features_deploy.py (部署链路 / 固件 C 特征)",
        "repeat": args.repeat,
        "holdout_repeat": args.holdout_repeat,
        "noise": os.path.abspath(os.path.expanduser(args.noise)),
        "train_voices": train_voices,
        "heldout_voices": held_voices,
        "pos_train_files": len(train),
        "pos_heldout_files": len(heldout),
        "counts": counts,
        "holdout_overlap": [],
    }
    with open(os.path.join(out_root, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, ensure_ascii=False, indent=2, sort_keys=True)
    print("OK: %s" % out_root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
