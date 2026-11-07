#!/usr/bin/env bash
# 启动 wakeword v17 训练（开发机上跑）。
#
#   ./run_training.sh --smoke     # 20 步冒烟，只验证链路，产物在 /tmp
#   ./run_training.sh             # 正式训练 12000 步 + 量化流式模型评估
#   ./run_training.sh --steps 3000 --no-eval
#
# 两个历史坑在这里统一处理：
#   1. microwakeword 的 editable 安装指向已搬家的旧路径，必须显式 PYTHONPATH；
#   2. 训练配置里 features_dir 是 ./features_xxx 相对路径，cwd 必须是 mww_data。
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MWW_DATA=${MWW_DATA:-"$HOME/theplace/XiaoZhiCam/mww_data"}
MWW_SRC=${MWW_SRC:-"$HOME/theplace/XiaoZhiCam/mww_setup/microWakeWord"}
PY=${PY:-"$HOME/miniconda3/envs/mww/bin/python"}
CONFIG=${CONFIG:-"$HERE/training_parameters_v17.yaml"}
LOG=${LOG:-"$MWW_DATA/train_v17.log"}

SMOKE=0
STEPS=""
TRAIN_DIR=""
EVAL_INTERVAL=""
TEST_QUANT=1
LOG_SET=0

usage() { sed -n '2,14p' "$0"; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --smoke) SMOKE=1; shift ;;
    --steps) STEPS=$2; shift 2 ;;
    --train-dir) TRAIN_DIR=$2; shift 2 ;;
    --eval-interval) EVAL_INTERVAL=$2; shift 2 ;;
    --no-eval) TEST_QUANT=0; shift ;;
    --log) LOG=$2; LOG_SET=1; shift 2 ;;
    -h|--help) usage ;;
    *) echo "未知参数: $1" >&2; usage ;;
  esac
done

if [ "$SMOKE" = 1 ]; then
  STEPS=${STEPS:-20}
  TRAIN_DIR=${TRAIN_DIR:-/tmp/wakeword_v17_smoke}
  EVAL_INTERVAL=${EVAL_INTERVAL:-5}
  TEST_QUANT=0
  [ "$LOG_SET" = 1 ] || LOG=/tmp/train_v17_smoke.log
  CFG=/tmp/training_parameters_v17_smoke.yaml
  "$PY" - "$CONFIG" "$CFG" "$STEPS" "$TRAIN_DIR" "$EVAL_INTERVAL" <<'PY'
import sys, yaml
src, dst, steps, train_dir, interval = sys.argv[1:6]
c = yaml.safe_load(open(src, encoding="utf-8"))
c["training_steps"] = [int(steps)]
c["train_dir"] = train_dir
c["eval_step_interval"] = int(interval)
yaml.safe_dump(c, open(dst, "w", encoding="utf-8"), sort_keys=False)
PY
  rm -rf "$TRAIN_DIR"
elif [ -n "$STEPS" ] || [ -n "$TRAIN_DIR" ] || [ -n "$EVAL_INTERVAL" ]; then
  CFG=/tmp/training_parameters_v17_custom.yaml
  "$PY" - "$CONFIG" "$CFG" "$STEPS" "$TRAIN_DIR" "$EVAL_INTERVAL" <<'PY'
import sys, yaml
src, dst, steps, train_dir, interval = sys.argv[1:6]
c = yaml.safe_load(open(src, encoding="utf-8"))
if steps: c["training_steps"] = [int(steps)]
if train_dir: c["train_dir"] = train_dir
if interval: c["eval_step_interval"] = int(interval)
yaml.safe_dump(c, open(dst, "w", encoding="utf-8"), sort_keys=False)
PY
else
  CFG=$CONFIG
fi

[ -d "$MWW_DATA" ] || { echo "找不到数据根目录 $MWW_DATA" >&2; exit 1; }
[ -d "$MWW_SRC/microwakeword" ] || { echo "找不到 microWakeWord 源码 $MWW_SRC" >&2; exit 1; }
mkdir -p "$MWW_DATA/trained_models" "$(dirname "$LOG")"

echo "配置 : $CFG"
echo "数据 : $MWW_DATA"
echo "日志 : $LOG"
echo "量化流式评估 : $TEST_QUANT"

cd "$MWW_DATA"
set +e
PYTHONPATH="$MWW_SRC" TF_CPP_MIN_LOG_LEVEL=2 TF_ENABLE_ONEDNN_OPTS=0 \
  "$PY" -m microwakeword.model_train_eval \
    --training_config="$CFG" \
    --train 1 \
    --restore_checkpoint 0 \
    --test_tf_nonstreaming 0 \
    --test_tflite_nonstreaming 0 \
    --test_tflite_nonstreaming_quantized 0 \
    --test_tflite_streaming 0 \
    --test_tflite_streaming_quantized "$TEST_QUANT" \
    --use_weights best_weights \
    mixednet \
      --pointwise_filters "64,64,64,64" \
      --repeat_in_block "1, 1, 1, 1" \
      --mixconv_kernel_sizes '[5], [7,11], [9,15], [23]' \
      --residual_connection "0,0,0,0" \
      --first_conv_filters 32 \
      --first_conv_kernel_size 5 \
      --stride 3 \
    > "$LOG" 2>&1
rc=$?
set -e
echo "训练退出码: $rc"
tail -n 25 "$LOG"
exit "$rc"
