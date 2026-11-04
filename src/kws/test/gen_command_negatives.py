#!/usr/bin/env python3
"""合成近音硬负样本（"像但不是唤醒词"）。

为什么要专门造这一批：v4/v5 重训后召回 100%，但误触发涨到每小时几十次——
模型没有"不像"的概念，只见过 33 条真实底噪。近音词是唤醒词模型的标准做法：
让模型学会区分「你好小智」和「你好小志 / 你好小明 / 小智小智」。

用 piper + zh_CN-huayan（与训练正样本同一个语音源），每条短语合成多条
（不同语速/噪声尺度），再统一降到 16 kHz，与训练管线一致。

用法：
    python gen_hard_negatives.py [--per 6] [--out ./data/cmd_neg]
"""
import argparse
import os
import shutil
import subprocess
import tempfile

import numpy as np
import scipy.io.wavfile as wavfile
from scipy.signal import resample_poly

HOME = os.path.expanduser("~")
PIPER_DIR = os.path.join(HOME, "mww_setup/piper-sample-generator")
VOICE = os.path.join(HOME, "mww_setup/zh_voice/zh_CN-huayan-medium.onnx")
PY = os.path.join(HOME, "miniconda3/envs/mww/bin/python")

# 覆盖四类"像但不是"：
#   1) 同音/近音字   2) 少字/多字/语序
#   3) 同节奏换名字   4) 其他常见唤醒词（防止"是个唤醒词就应"）
PHRASES = [
    "讲个故事", "讲个笑话", "给我放首歌", "播放音乐", "暂停一下", "下一首",
    "现在几点了", "今天天气怎么样", "明天会下雨吗", "今天几号", "明天星期几",
    "开个灯", "关灯", "把灯调亮一点", "把空调调到二十六度", "打开电视", "关掉电视",
    "拉上窗帘", "打开风扇", "我要睡觉了", "我回来了", "我出门了", "晚安", "早上好",
    "提醒我明天七点起床", "定个十分钟的闹钟", "中午吃什么", "我饿了", "给我倒杯水",
    "今天好累啊", "这周过得真快", "明天要开会", "我快到家了", "这个东西怎么用",
    "你是谁", "你会做什么", "帮我想个办法", "查一下天气", "导航去公司", "打电话回家",
    "声音大一点", "声音小一点", "唱首歌吧", "把音量调大", "现在多少度", "几点了",
    "今天心情不错", "帮我查个单词", "翻译一下", "给我念一首诗", "来个脑筋急转弯",
    "什么是人工智能", "帮我记一下", "明天提醒我买牛奶", "路口左转", "菜价多少钱",
    "你好小志", "你好小治", "你好小致", "你好小稚", "你好小至", "你好小知",
    "你好晓智", "你好小智智", "你好小智呀",
    "你好", "你好呀", "你好好", "您好小智", "你号小智",
    "小智", "小智小智", "小智你好", "嘿小智", "喂小智",
    "你好小明", "你好小丽", "你好小艺", "你好小琪",
    "小爱同学", "小度小度", "天猫精灵", "你好小艺",
]


def resample_to_16k(src, dst):
    rate, x = wavfile.read(src)
    if x.ndim > 1:
        x = x[:, 0]
    if rate != 16000:
        g = np.gcd(rate, 16000)
        x = resample_poly(x.astype(np.float32), 16000 // g, rate // g)
    x = np.clip(np.round(x), -32768, 32767).astype("<i2")
    wavfile.write(dst, 16000, x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per", type=int, default=6, help="每条短语合成多少条")
    ap.add_argument("--out", default=os.path.join(HOME, "data/cmd_neg"))
    args = ap.parse_args()

    out_dir = os.path.expanduser(args.out)
    os.makedirs(out_dir, exist_ok=True)

    total = 0
    for pi, phrase in enumerate(PHRASES):
        tmp = tempfile.mkdtemp(prefix="piper_")
        try:
            subprocess.run(
                [PY, "-m", "piper_sample_generator", phrase,
                 "--max-samples", str(args.per), "--model", VOICE,
                 "--output-dir", tmp],
                cwd=PIPER_DIR, check=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            files = sorted(f for f in os.listdir(tmp) if f.endswith(".wav"))
            for i, f in enumerate(files):
                dst = os.path.join(out_dir, "cmd%03d_%02d.wav" % (pi, i))
                resample_to_16k(os.path.join(tmp, f), dst)
                total += 1
            print("%-8s %2d 条" % (phrase, len(files)), flush=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    print("合计 %d 条 -> %s" % (total, out_dir))


if __name__ == "__main__":
    main()
