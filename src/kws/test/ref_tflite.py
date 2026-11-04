#!/usr/bin/env python3
"""TFLite 独立参考推理（只依赖 ai_edge_litert，不需要 flatbuffers/tflite 包）。

用 gold 数据里的 40 维 int8 特征喂流式模型，逐帧输出概率（每 3 帧一次）。
这是"C 实现 → C++ 实现 → 官方 TFLite"三段对拍里的最后一环：
C 与 C++ 逐位相同只能证明移植忠实，还必须证明 C++ 本身没理解错模型。

用法（开发机）：
    ~/miniconda3/envs/mww/bin/python ref_tflite.py <model.tflite> <frames.bin> <out.prob>

输出：单个 uint8 序列文件，与 kws_host prob 的输出格式一致，可直接 cmp。
"""
import sys

import numpy as np
from ai_edge_litert.interpreter import Interpreter, OpResolverType


def main():
    model_path, frames_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

    raw = np.fromfile(frames_path, dtype=np.int8)
    n = raw.size // 40
    frames = raw[: n * 40].reshape(n, 40)

    interp = Interpreter(model_path=model_path,
                         experimental_op_resolver_type=OpResolverType.BUILTIN_REF)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]
    print(f"输入 {inp['name']} {inp['shape']} {inp['dtype']}")
    print(f"输出 {out['name']} {out['shape']} {out['dtype']}  共 {n} 帧")

    try:
        interp.reset_all_variables()
    except Exception as e:  # 老版本解释器没有这个 API
        print(f"(reset_all_variables 不可用：{e})")

    # 模型输入是 [1,3,40]：一次吃 3 个 10 ms 帧，对应固件里"攒够 3 帧跑一次"
    probs = bytearray()
    for i in range(0, n - 2, 3):
        interp.set_tensor(inp["index"], frames[i:i + 3].reshape(inp["shape"]))
        interp.invoke()
        probs.append(int(interp.get_tensor(out["index"]).reshape(-1)[0]) & 0xFF)

    with open(out_path, "wb") as f:
        f.write(probs)
    print(f"已写出 {out_path}（{len(probs)} 个概率）")


if __name__ == "__main__":
    main()
