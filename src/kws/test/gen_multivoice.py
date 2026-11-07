#!/usr/bin/env python3
"""用多音色 TTS 生成「该醒 / 不该醒」成对素材。

设计要点（docs/wakeword-retrain-guide.md）：
  * 正样本过去只有 1 个 piper 音色 + 48 条真人，模型容易学成「听见真人说话就醒」。
    这里用 edge-tts 的多个 zh-CN 音色扩充说话人多样性，成本近乎为零。
  * **同一批音色既说「你好小智」，也说一堆不相干句子**。否则正负样本的音色
    分布不一致，模型会走捷径学音色而不是学词。
  * 仍是合成域：离线考卷用「留出未参与训练的 AI 音色 + AISHELL 留出说话人」，
    与训练零交集；全程不需要真人录音。

产出：
  out/positive/<voice>/pNNN.wav   1.8 s，词从开头（供 gen_features_deploy.py）
  out/negative/<voice>/nNNN.wav   正常句子
  out/manifest.json               音色/文本/语速/seed/文件清单，可复现

  可选 --negative-features 直接把负句转成固件同分布特征，按音色分组。

依赖：开发机 conda 环境 mww 里的 edge_tts；音频解码用清单 §5 的 ~/bin/ffmpeg。

用法：
  ~/miniconda3/envs/mww/bin/python gen_multivoice.py \
      --out ~/theplace/XiaoZhiCam/mww_data/multivoice_v1 --pos-per-voice 6 --neg-per-voice 24

  冒烟：--voices 2 --pos-per-voice 1 --neg-per-voice 1 --out /tmp/mv_smoke
"""
import argparse
import asyncio
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus_common as cc  # noqa: E402

DEFAULT_MWW = "~/theplace/XiaoZhiCam/mww_data"

POS_TEXTS = ["你好小智", "你好小智。", "你好，小智", "你好小智？"]

NEG_TEXTS = [
    "现在几点了", "几点了", "今天几号", "明天星期几", "今天天气怎么样",
    "明天会下雨吗", "现在多少度", "查一下天气", "讲个故事", "讲个笑话",
    "给我放首歌", "播放音乐", "暂停一下", "下一首", "声音大一点",
    "把音量调小", "开个灯", "关灯", "把灯调亮一点", "打开电视",
    "拉上窗帘", "打开风扇", "我要睡觉了", "我回来了", "我出门了",
    "晚安", "早上好", "提醒我明天七点起床", "定个十分钟的闹钟",
    "中午吃什么", "这个东西怎么用", "你是谁", "你会做什么",
    "帮我想个办法", "导航去公司", "今天好累啊", "明天要开会",
    "我快到家了", "帮我查个单词", "翻译一下", "给我念一首诗",
    "什么是人工智能", "路口左转", "菜价多少钱",
]

RATES = ["-10%", "+0%", "+10%"]


def trim_leading_silence(x, thresh=0.008, frame=160):
    for i in range(0, max(1, len(x) - frame), frame):
        if float(np.sqrt(np.mean(x[i:i + frame] ** 2))) > thresh:
            return x[max(0, i - frame):]
    return x


def fit_positive(x, seconds=1.8):
    x = trim_leading_silence(x)
    n = int(seconds * 16000)
    if len(x) >= n:
        return x[:n]
    return np.pad(x, (0, n - len(x)))


async def tts_bytes(text, voice, rate, retries=3, timeout=60):
    import edge_tts
    last = None
    for attempt in range(retries):
        try:
            comm = edge_tts.Communicate(text, voice, rate=rate)
            buf = bytearray()

            async def collect():
                async for chunk in comm.stream():
                    if chunk.get("type") == "audio":
                        buf.extend(chunk.get("data", b""))

            await asyncio.wait_for(collect(), timeout=timeout)
            if buf:
                return bytes(buf)
            last = RuntimeError("空音频")
        except Exception as e:
            last = e
        await asyncio.sleep(1.5 * (attempt + 1))
    raise RuntimeError("TTS 失败 %s/%s rate=%s: %s" % (voice, text, rate, last))


async def available_voices():
    import edge_tts
    vs = await edge_tts.list_voices()
    zh = [v["ShortName"] for v in vs if v.get("Locale", "").startswith("zh-CN")]
    return sorted(set(zh))


def voice_tag(name):
    return name.replace("zh-CN-", "").replace("-", "_")


async def generate(args, voices):
    out_root = os.path.expanduser(args.out)
    pos_root = os.path.join(out_root, "positive")
    neg_root = os.path.join(out_root, "negative")
    os.makedirs(pos_root, exist_ok=True)
    os.makedirs(neg_root, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    holdout = set(voices[len(voices) - args.holdout_voices:]) if args.holdout_voices else set()
    manifest = {"voices": voices, "heldout_voices": sorted(holdout),
                "rates": RATES, "seed": args.seed,
                "pos_texts": POS_TEXTS, "neg_texts": NEG_TEXTS, "files": []}

    for vi, voice in enumerate(voices):
        split_tag = "heldout" if voice in holdout else "train"
        tag = voice_tag(voice)
        pd = os.path.join(pos_root, tag)
        nd = os.path.join(neg_root, tag)
        os.makedirs(pd, exist_ok=True)
        os.makedirs(nd, exist_ok=True)

        # 正样本：每人 pos-per-voice 条，轮换文本与语速
        for k in range(args.pos_per_voice):
            rel = os.path.join(tag, "p%03d.wav" % k)
            path = os.path.join(pos_root, rel)
            text = POS_TEXTS[k % len(POS_TEXTS)]
            rate = RATES[(k // len(POS_TEXTS)) % len(RATES)]
            if args.resume and os.path.isfile(path):
                manifest["files"].append({"path": "positive/" + rel, "voice": voice,
                                         "label": 1, "text": text, "rate": rate, "split": split_tag})
                continue
            data = await tts_bytes(text, voice, rate)
            audio = fit_positive(cc.decode_audio_bytes(data))
            cc.write_wav16k(path, audio)
            manifest["files"].append({"path": "positive/" + rel, "voice": voice,
                                     "label": 1, "text": text, "rate": rate, "split": split_tag})
            print("  + %s p%03d  %s" % (tag, k, text))

        # 负样本：同一批音色，说不相干的句子
        texts = list(NEG_TEXTS)
        rng.shuffle(texts)
        for k in range(args.neg_per_voice):
            rel = os.path.join(tag, "n%03d.wav" % k)
            path = os.path.join(neg_root, rel)
            text = texts[k % len(texts)]
            rate = RATES[(k // len(texts) + 1) % len(RATES)] if len(RATES) else "+0%"
            if args.resume and os.path.isfile(path):
                manifest["files"].append({"path": "negative/" + rel, "voice": voice,
                                         "label": 0, "text": text, "rate": rate, "split": split_tag})
                continue
            data = await tts_bytes(text, voice, rate)
            audio = cc.decode_audio_bytes(data)
            cc.write_wav16k(path, audio)
            manifest["files"].append({"path": "negative/" + rel, "voice": voice,
                                     "label": 0, "text": text, "rate": rate, "split": split_tag})
            print("  - %s n%03d  %s" % (tag, k, text))
        print("[%d/%d] %s 完成" % (vi + 1, len(voices), tag))

    cc.write_manifest(os.path.join(out_root, "manifest.json"), manifest)
    return out_root, manifest


def build_negative_features(args, out_root, manifest):
    """把负句 WAV 转成固件同分布特征，按音色分组（测试集音色完全独立）。"""
    cc.setup_deploy(args.noise)
    rng = np.random.default_rng(args.seed + 1)
    by_voice = {}
    for rec in manifest["files"]:
        if rec["label"] != 0:
            continue
        path = os.path.join(out_root, rec["path"])
        try:
            feats = cc.to_features(cc.read_wav16k(path), rng)
            wins = cc.cut_windows(feats)
        except Exception as e:
            print("  [warn] %s: %s" % (path, e), file=sys.stderr)
            continue
        by_voice.setdefault(rec["voice"], []).extend(wins)
    holdout = set(manifest.get("heldout_voices", []))
    train_names = sorted(v for v in by_voice if v not in holdout)
    test_names = sorted(v for v in by_voice if v in holdout)
    feat_root = os.path.join(out_root, "features")

    if test_names:
        # 考卷只由「留出音色」组成，与训练音色零交集；全程无真人录音
        rng2 = np.random.default_rng(args.seed)
        shuffled = list(train_names)
        rng2.shuffle(shuffled)
        n_val = max(1, len(shuffled) // 5) if len(shuffled) > 1 else 0
        splits = {
            "training": [w for v in shuffled[n_val:] for w in by_voice[v]],
            "validation": [w for v in shuffled[:n_val] for w in by_voice[v]],
            "testing": [w for v in test_names for w in by_voice[v]],
        }
    else:
        if len(train_names) < 3:
            print("[warn] 只有 %d 个音色，跳过按音色分组的特征输出" % len(train_names))
            return
        groups = cc.speaker_groups(train_names, args.seed)
        splits = {s: [w for v in vs for w in by_voice.get(v, [])]
                  for s, vs in groups.items()}

    for split, ws in splits.items():
        n = cc.write_split(feat_root, split, ws)
        note = "  [留出音色]" if split == "testing" and test_names else ""
        print("features/%-10s: %d 窗口%s" % (split, n, note))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="输出根目录")
    ap.add_argument("--voices", default="", help="逗号分隔的 edge-tts 音色名；留空=全部 zh-CN")
    ap.add_argument("--voice-count", type=int, default=0, help="只用前 N 个音色（冒烟用）")
    ap.add_argument("--holdout-voices", type=int, default=2,
                    help="末尾 N 个音色留作考卷，只进 testing，不进训练")
    ap.add_argument("--pos-per-voice", type=int, default=6)
    ap.add_argument("--neg-per-voice", type=int, default=24)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--resume", action="store_true", help="跳过已存在的 WAV")
    ap.add_argument("--negative-features", action="store_true",
                    help="负句直接产出固件特征（按音色分组）")
    ap.add_argument("--noise", default=DEFAULT_MWW + "/noise30m.raw")
    args = ap.parse_args()

    voices = ([v.strip() for v in args.voices.split(",") if v.strip()]
              if args.voices else asyncio.run(available_voices()))
    if not voices:
        raise SystemExit("没有可用的中文音色")
    if args.voice_count:
        voices = voices[:args.voice_count]
    print("音色 %d 个: %s" % (len(voices), ", ".join(voices)))
    print("ffmpeg: %s" % cc.find_ffmpeg())

    t0 = time.time()
    out_root, manifest = asyncio.run(generate(args, voices))
    print("WAV 完成：%d 个（%.1f s）" % (len(manifest["files"]), time.time() - t0))
    if args.negative_features:
        build_negative_features(args, out_root, manifest)


if __name__ == "__main__":
    main()
