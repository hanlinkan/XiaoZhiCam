#!/usr/bin/env python3
"""用多音色 TTS 生成「该醒 / 不该醒」成对素材。

设计要点（见本地训练笔记，仓库不分发）：
  * 正样本过去只有 1 个 piper 音色 + 48 条真人，模型容易学成「听见真人说话就醒」。
    这里用 edge-tts 的多个 zh-CN 音色扩充说话人多样性，成本近乎为零。
  * **同一批音色既说「你好小智」，也说一堆不相干句子**。否则正负样本的音色
    分布不一致，模型会走捷径学音色而不是学词。
  * 仍是合成域：离线考卷用「留出未参与训练的 AI 音色 + AISHELL 留出说话人」，
    与训练零交集；全程不需要真人录音。
  * v18 扩充：文本 / 语速 / 音高 / 音量四维韵律组合，正负样本用**同一套**维度，
    避免模型靠「韵律丰富 = 该醒」偷懒；`--holdout-list` 可锁定留出音色，
    保证跨版本判卷用的是同一批「没见过的人」。

产出：
  out/positive/<voice>/pNNN.wav   1.8 s，词从开头（供 gen_features_deploy.py）
  out/negative/<voice>/nNNN.wav   正常句子
  out/manifest.json               音色/文本/语速/seed/文件清单，可复现

  可选 --negative-features 直接把负句转成固件同分布特征，按音色分组。

依赖：开发机 conda 环境 mww 里的 edge_tts；音频解码用清单 §5 的 ~/bin/ffmpeg。

用法：
  ~/miniconda3/envs/mww/bin/python gen_multivoice.py \
      --out ~/theplace/XiaoZhiCam/mww_data/multivoice_v2 \
      --locales zh-CN,zh-TW --pos-per-voice 12 --neg-per-voice 12 \
      --holdout-list zh-CN-liaoning-XiaobeiNeural,zh-CN-shaanxi-XiaoniNeural \
      --negative-features

  冒烟：--voice-count 2 --pos-per-voice 1 --neg-per-voice 1 --out /tmp/mv_smoke
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

# 只变标点和停顿，唤醒词本体一字不改 —— 换词序会换掉标签语义
POS_TEXTS = [
    "你好小智", "你好小智。", "你好，小智", "你好小智？",
    "你好小智！", "你好 小智", "你好，小智。", "你好，小智？",
]

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
PITCHES = ["-20Hz", "+0Hz", "+20Hz"]
VOLUMES = ["-10%", "+0%", "+10%"]

# 8 文本 × 3 语速 × 3 音高 × 3 音量 = 216 种确定性组合，
# 每个说话人各抽一组不重复的子集，覆盖比 v17 的「4 文本 × 3 语速」宽得多。
def prosody_combos():
    return [(t, r, p, v) for t in POS_TEXTS
            for r in RATES for p in PITCHES for v in VOLUMES]


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


async def tts_bytes(text, voice, rate, pitch="+0Hz", volume="+0%",
                    retries=3, timeout=60):
    import edge_tts
    last = None
    for attempt in range(retries):
        try:
            comm = edge_tts.Communicate(text, voice, rate=rate,
                                       volume=volume, pitch=pitch)
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
    raise RuntimeError("TTS 失败 %s/%s rate=%s pitch=%s vol=%s: %s" %
                       (voice, text, rate, pitch, volume, last))


async def available_voices(locales):
    import edge_tts
    vs = await edge_tts.list_voices()
    names = [v["ShortName"] for v in vs
             if any(v.get("Locale", "").startswith(loc) for loc in locales)]
    return sorted(set(names))


def voice_tag(name):
    return name.replace("zh-CN-", "").replace("-", "_")


async def generate(args, voices):
    out_root = os.path.expanduser(args.out)
    pos_root = os.path.join(out_root, "positive")
    neg_root = os.path.join(out_root, "negative")
    os.makedirs(pos_root, exist_ok=True)
    os.makedirs(neg_root, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    if args.holdout_list:
        holdout = {v.strip() for v in args.holdout_list.split(",") if v.strip()}
        missing = sorted(holdout - set(voices))
        if missing:
            raise SystemExit("留出音色不在可用列表里: %s" % missing)
    elif args.holdout_voices:
        holdout = set(voices[len(voices) - args.holdout_voices:])
    else:
        holdout = set()
    manifest = {"voices": voices, "heldout_voices": sorted(holdout),
                "locales": args.locales, "rates": RATES, "pitches": PITCHES,
                "volumes": VOLUMES, "seed": args.seed,
                "pos_texts": POS_TEXTS, "neg_texts": NEG_TEXTS, "files": []}

    combos = prosody_combos()
    if args.pos_per_voice > len(combos):
        raise SystemExit("--pos-per-voice 不能超过韵律组合数 %d" % len(combos))

    for vi, voice in enumerate(voices):
        split_tag = "heldout" if voice in holdout else "train"
        tag = voice_tag(voice)
        pd = os.path.join(pos_root, tag)
        nd = os.path.join(neg_root, tag)
        os.makedirs(pd, exist_ok=True)
        os.makedirs(nd, exist_ok=True)

        # 每个说话人抽一组不同的韵律组合，避免所有人的「你好小智」都一个味儿
        picks = rng.permutation(len(combos))[:args.pos_per_voice]
        for k, ci in enumerate(picks):
            text, rate, pitch, volume = combos[int(ci)]
            rel = os.path.join(tag, "p%03d.wav" % k)
            path = os.path.join(pos_root, rel)
            rec = {"path": "positive/" + rel, "voice": voice, "label": 1,
                   "text": text, "rate": rate, "pitch": pitch, "volume": volume,
                   "split": split_tag}
            if args.resume and os.path.isfile(path):
                manifest["files"].append(rec)
                continue
            data = await tts_bytes(text, voice, rate, pitch, volume)
            audio = fit_positive(cc.decode_audio_bytes(data))
            cc.write_wav16k(path, audio)
            manifest["files"].append(rec)
            print("  + %s p%03d %s r=%s p=%s v=%s" %
                  (tag, k, text, rate, pitch, volume))

        # 负样本：同一批音色 + 同样的韵律维度，防止模型靠「韵律丰富=正样本」偷懒
        texts = list(NEG_TEXTS)
        rng.shuffle(texts)
        for k in range(args.neg_per_voice):
            rel = os.path.join(tag, "n%03d.wav" % k)
            path = os.path.join(neg_root, rel)
            text = texts[k % len(texts)]
            rate = RATES[(k // len(texts) + 1) % len(RATES)]
            pitch = PITCHES[(k // len(RATES)) % len(PITCHES)]
            volume = VOLUMES[(k // (len(RATES) * len(PITCHES))) % len(VOLUMES)]
            rec = {"path": "negative/" + rel, "voice": voice, "label": 0,
                   "text": text, "rate": rate, "pitch": pitch, "volume": volume,
                   "split": split_tag}
            if args.resume and os.path.isfile(path):
                manifest["files"].append(rec)
                continue
            data = await tts_bytes(text, voice, rate, pitch, volume)
            audio = cc.decode_audio_bytes(data)
            cc.write_wav16k(path, audio)
            manifest["files"].append(rec)
            print("  - %s n%03d  %s" % (tag, k, text))
        print("[%d/%d] %s 完成" % (vi + 1, len(voices), tag))

    cc.write_manifest(os.path.join(out_root, "manifest.json"), manifest)
    return out_root, manifest


def build_negative_features(args, out_root, manifest):
    """把负句 WAV 转成固件同分布特征，按音色分组（测试集音色完全独立）。

    负句必须和正样本走**同样的房间/噪声域**：正样本经 gen_features_deploy.py
    的 Augmentation 加了 RIR 与背景噪声，若负句是干净的 TTS，模型最省力的解法
    会是「带噪合成短句=该醒、干净合成短句=不该醒」，真机上依旧乱醒。
    """
    cc.setup_deploy(args.noise)
    rng = np.random.default_rng(args.seed + 1)
    rirs = cc.load_rirs(args.rirs) if args.rir_prob > 0 else []
    noise16k = cc.load_noise16k(args.noise) if args.snr_max > args.snr_min else None
    print("负句增强：RIR %d 个 / 概率 %.2f / SNR %.0f~%.0f dB" %
          (len(rirs), args.rir_prob, args.snr_min, args.snr_max))
    by_voice = {}
    for rec in manifest["files"]:
        if rec["label"] != 0:
            continue
        path = os.path.join(out_root, rec["path"])
        try:
            audio = cc.read_wav16k(path)
            if rirs and rng.random() < args.rir_prob:
                audio = cc.apply_rir(audio, rirs[int(rng.integers(0, len(rirs)))][1])
            if noise16k is not None:
                audio = cc.add_noise(audio, noise16k,
                                     float(rng.uniform(args.snr_min, args.snr_max)), rng)
            feats = cc.to_features(audio, rng)
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
    ap.add_argument("--voices", default="", help="逗号分隔的 edge-tts 音色名；留空按 --locales 取")
    ap.add_argument("--locales", default="zh-CN",
                    help="逗号分隔的 locale 前缀，如 zh-CN,zh-TW（zh-HK 是粤语，不建议做正样本）")
    ap.add_argument("--voice-count", type=int, default=0, help="只用前 N 个音色（冒烟用）")
    ap.add_argument("--holdout-voices", type=int, default=2,
                    help="末尾 N 个音色留作考卷，只进 testing，不进训练")
    ap.add_argument("--holdout-list", default="",
                    help="显式指定留出音色（逗号分隔）；跨版本对比时必须用它锁定同一批人")
    ap.add_argument("--pos-per-voice", type=int, default=6)
    ap.add_argument("--neg-per-voice", type=int, default=24)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--resume", action="store_true", help="跳过已存在的 WAV")
    ap.add_argument("--negative-features", action="store_true",
                    help="负句直接产出固件特征（按音色分组）")
    ap.add_argument("--noise", default=DEFAULT_MWW + "/noise30m.raw")
    ap.add_argument("--rirs", default=DEFAULT_MWW + "/mit_rirs", help="房间脉冲响应目录")
    ap.add_argument("--rir-prob", type=float, default=0.5, help="负句加房间混响的概率")
    ap.add_argument("--snr-min", type=float, default=5.0, help="负句叠真机底噪 SNR 下限 dB")
    ap.add_argument("--snr-max", type=float, default=20.0, help="负句叠真机底噪 SNR 上限 dB")
    args = ap.parse_args()

    locales = [x.strip() for x in args.locales.split(",") if x.strip()]
    voices = ([v.strip() for v in args.voices.split(",") if v.strip()]
              if args.voices else asyncio.run(available_voices(locales)))
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
