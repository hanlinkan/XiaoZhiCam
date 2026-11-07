#!/bin/sh
# solo16k.sh —— 16k 独立模式包装（仅在卡上存在 .solo16k 标记时生效）
#
# 背景（详见 XiaoZhiCam-OSS/.workbuddy/memory/2026-11-07.md）
#   1. 用户态 MPP 的内核侧初始化由原厂 rmm 完成；rmm 从未跑过时
#      HI_MPI_SYS_Init 返回 0xA0028010 (EN_ERR_SYS_NOTREADY)。
#   2. 但 SIGKILL 不走 SYS_Exit，内核 MPP 状态会保留，之后仍可 SYS_Init。
#   3. rmm 启动时会 SETTIMEOUT(10)，其 fd 关闭时 hidog_release 又会用
#      cur_margin(=10) 重设一次。于是硬件 10 秒到期，而 hidog daemon 每
#      30 秒才喂一次 -> 复位。直写 LOAD=60s 可让 daemon 的节奏够用，
#      但因为 release 会覆盖一次，必须在 kill 之后重写。
#   4. AI_SetPubAttr 改的是静态属性，通道一旦被 Enable 过就返回 NOT_PERM，
#      所以必须在 p8s 之前把 rmm 杀掉，且不要手动 Disable（会把状态弄脏）。
#
# 本脚本只做两件事：停 rmm、把看门狗顶到 60 秒，然后交给 launch_p8.sh。
# 标记不存在时完全是透明转发，行为与原方案一致。

HACK=/tmp/hd1/hack

if [ -f "$HACK/.solo16k" ]; then
    killall -9 rmm 2>/dev/null
    sleep 1
    # DesignWare WDT：解锁 -> LOAD=180,000,000 (60s @3MHz) -> 上锁
    busybox devmem 0x20040C00 32 0x1ACCE551
    busybox devmem 0x20040000 32 0x0ABA9500
    busybox devmem 0x20040C00 32 0
    echo "solo16k: rmm stopped, wdt LOAD set to 60s" >> /tmp/xz.log
fi

exec sh $HACK/launch_p8.sh
