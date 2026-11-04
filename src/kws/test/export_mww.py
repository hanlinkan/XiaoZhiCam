#!/usr/bin/env python3
"""从 TFLite 导出 mww_model.h（通用版，不写死任何量化参数）。

上一版的 export_engine.py 把每层的 out_scale / out_zp / tensor 编号全部写死，
换一个训练结果就不对了。这里改成**按算子顺序**从模型里读：

    for op in interpreter._get_ops_details():
        CONV_2D / DEPTHWISE_CONV_2D / FULLY_CONNECTED -> 一层

每层取输入/输出量化参数、int8 权重、int32 bias，再按 gemmlowp 的
QuantizeMultiplier 算每通道 Q31 乘数。输出的头文件格式与固件一致。

自检：对 v2 的 tflite 跑一遍，结果应与仓库里现有的 mww_model.h 逐字节相同。

用法：
    python export_mww.py <model.tflite> <out.h>
"""
import os
import sys

import numpy as np
import tensorflow as tf


def quantize_multiplier(mult):
    if mult == 0.0:
        return 0, 0
    q, exp = np.frexp(mult)          # q ∈ [0.5, 1)
    q = int(round(q * (1 << 31)))
    if q == (1 << 31):
        q //= 2
        exp += 1
    if exp < -31:
        q, exp = 0, 0
    return q, int(exp)


def emit_i8(f, name, arr):
    arr = np.asarray(arr, dtype=np.int8).reshape(-1)
    f.write("static const int8_t %s[%d] = {\n" % (name, arr.size))
    for i in range(0, arr.size, 16):
        f.write("  " + ",".join(str(int(v)) for v in arr[i:i + 16]) + ",\n")
    f.write("};\n")


def emit_i32(f, name, arr):
    arr = np.asarray(arr, dtype=np.int64).reshape(-1)
    f.write("static const int32_t %s[%d] = {\n" % (name, arr.size))
    for i in range(0, arr.size, 8):
        f.write("  " + ",".join(str(int(v)) for v in arr[i:i + 8]) + ",\n")
    f.write("};\n")


def main():
    model_path, out_path = sys.argv[1], sys.argv[2]

    it = tf.lite.Interpreter(model_path=model_path)
    it.allocate_tensors()
    det = it.get_tensor_details()

    def qparams(idx):
        q = det[idx]["quantization_parameters"]
        return (np.array(q["scales"], dtype=np.float64),
                np.array(q["zero_points"], dtype=np.int64))

    # 按执行顺序挑出卷积类算子
    layers = []
    for op in it._get_ops_details():
        name = op["op_name"]
        if name not in ("CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED"):
            continue
        li, lo = op["inputs"], op["outputs"]
        w_idx, b_idx = li[1], li[2]
        in_scale, in_zp = qparams(li[0])
        out_scale, out_zp = qparams(lo[0])
        w_scale, w_zp = qparams(w_idx)

        weights = it.get_tensor(w_idx).astype(np.int8).reshape(-1)
        bias = it.get_tensor(b_idx).astype(np.int64).reshape(-1)
        shape = it.get_tensor(w_idx).shape

        # 注意 tflite 的权重是 4 维：conv=[out,k,1,in]、dw=[1,k,1,ch]
        if name == "DEPTHWISE_CONV_2D":
            # [1, k, 1, ch] -> 固件按 [k][ch]（展平后天然就是 k*ch）
            k, ch = shape[1], shape[3]
            out_ch, in_ch = ch, 1
            kind = "dw"
            assert len(w_scale) == out_ch, (name, w_scale.shape)
        elif name == "FULLY_CONNECTED":
            # 固件把 FC 当成"k 个时间步 × 1 通道"：b5 是 [17][64] 展平成的 1088 维
            out_ch, k = shape[0], shape[1]
            in_ch = 1
            kind = "fc"
            assert len(w_scale) == out_ch
        else:
            out_ch, k, in_ch = shape[0], shape[1], shape[3]
            kind = "conv"
            assert len(w_scale) == out_ch, (name, w_scale.shape)
        assert (w_zp == 0).all(), "权重 zero_point 必须为 0"

        mults, shifts = [], []
        for c in range(out_ch):
            eff = float(in_scale[0]) * float(w_scale[c]) / float(out_scale[0])
            m, s = quantize_multiplier(eff)
            mults.append(m)
            shifts.append(s)

        layers.append(dict(name="L%d" % (len(layers) + 1), kind=kind, out_ch=out_ch, k=k,
                           in_ch=in_ch, in_zp=int(in_zp[0]), out_zp=int(out_zp[0]),
                           out_scale=float(out_scale[0]), weights=weights, bias=bias,
                           mults=np.array(mults), shifts=np.array(shifts)))

    if len(layers) < 10:
        raise SystemExit("只找到 %d 层，模型结构不对" % len(layers))

    # logistic 的输入域来自最后一层 FC 的输出量化
    fc = layers[-1]
    log_zp, log_scale = fc["out_zp"], fc["out_scale"]

    names, nc, nd = [], 0, 0
    for L in layers:
        if L["kind"] == "fc":
            names.append("fc")
        elif L["kind"] == "dw":
            nd += 1
            names.append("dw%d" % nd)
        else:
            nc += 1
            names.append("conv%d" % nc)

    with open(out_path, "w") as f:
        f.write("/* 自动生成，勿手改 —— 由 src/kws/test/export_mww.py 从 tflite 导出。\n")
        f.write(" *\n")
        f.write(" * 每层给三样东西：\n")
        f.write(" *   w     int8 权重，布局 conv=[out][k][in]、dw=[k][ch]、fc=[out][in]\n")
        f.write(" *   b     int32 bias\n")
        f.write(" *   mult  Q31 乘数（每输出通道）、shift 右移位数（每输出通道，可为负=左移）\n")
        f.write(" * 运行时：acc = Σ (a - in_zp) * w + b；out = clamp(MulByQ31(acc, mult, shift) + out_zp)\n")
        f.write(" */\n")
        f.write("#ifndef MWW_MODEL_H\n#define MWW_MODEL_H\n#include <stdint.h>\n\n")

        for nm, L in zip(names, layers):
            f.write("/* ---- %s: %s, out_ch=%d, k=%d, in_ch=%d ---- */\n"
                    % (nm, L["kind"], L["out_ch"], L["k"], L["in_ch"]))
            f.write("/* in_zp=%d out_scale=%.9g out_zp=%d */\n"
                    % (L["in_zp"], L["out_scale"], L["out_zp"]))
            emit_i8(f, "mww_w_" + nm, L["weights"])
            emit_i32(f, "mww_b_" + nm, L["bias"])
            emit_i32(f, "mww_m_" + nm, L["mults"])
            emit_i32(f, "mww_s_" + nm, L["shifts"])

        q = np.arange(-128, 128)
        p = 1.0 / (1.0 + np.exp(-((q - log_zp) * log_scale)))
        lut = np.clip(np.round(p * 256.0) - 128.0, -128, 127).astype(np.int8)
        f.write("/* ---- LOGISTIC：int8 -> int8 查表（精确 sigmoid） ---- */\n")
        emit_i8(f, "mww_logistic", lut)
        f.write("#define MWW_LOGISTIC_OUT_ADD 128\n")

        f.write("/* ---- 层表：顺序即执行顺序 ---- */\n")
        f.write("#define MWW_LAYER_COUNT %d\n" % len(layers))
        f.write("typedef struct {\n")
        f.write("    const int8_t* w; const int32_t* b; const int32_t* mult; const int32_t* shift;\n")
        f.write("    int16_t out_ch, k, in_ch; int16_t in_zp, out_zp; uint8_t is_dw;\n")
        f.write("} MwwLayer;\n\n")
        f.write("static const MwwLayer kMwwLayers[] = {\n")
        for nm, L in zip(names, layers):
            f.write("    { mww_w_%s, mww_b_%s, mww_m_%s, mww_s_%s, %d, %d, %d, %d, %d, %d },\n"
                    % (nm, nm, nm, nm, L["out_ch"], L["k"], L["in_ch"],
                       L["in_zp"], L["out_zp"], 1 if L["kind"] == "dw" else 0))
        f.write("};\n\n")
        f.write("#define MWW_FC_IN %d\n" % layers[-1]["k"])
        f.write("#define MWW_LOGISTIC_IN_ZP %d\n" % log_zp)
        f.write("#define MWW_LOGISTIC_SCALE %.8ff\n" % log_scale)
        f.write("#endif\n")

    print("写出 %s（%d 层，%.1f KB）" % (out_path, len(layers), os.path.getsize(out_path) / 1024.0))


if __name__ == "__main__":
    main()
