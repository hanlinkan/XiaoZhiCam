#!/bin/sh
# ============================================================================
#  XiaoZhiCam —— SD 卡引导钩子 v5
# ============================================================================
#  原理：原厂 /home/init.sh 里有个出厂测试钩子——
#
#      if [ -f "/home/hd1/test/equip_test.sh" ]; then
#              /home/hd1/test/equip_test.sh
#              exit
#      fi
#
#  插着这张卡开机时，钩子命中，本脚本以 root 身份执行，然后 exit 掉原厂流程。
#  本脚本再把 init.sh 守卫之后的"正常启动"部分原样跑一遍，于是相机保持完整
#  功能（WiFi、录像、米家 App 都正常），同时把语音助手拉起来。
#
#  v5 相对 v4 的改动：
#    改为按固件版本分流。版本匹配时用 SD 上的 init_norm.sh（它含三项针对语音
#    助手的改动：rmm 启动守护、停录像、停云服务）；版本不同则从本机
#    /home/init.sh 现场裁剪并补上停录像/停云服务，保证换一台相机也能跑。
#
#  安全性：不刷机、不动分区表、不改固件。拔掉 SD 卡 → 还原原厂启动行为。
#          注意运行时仍有少量 jffs2 写入（原厂启动流程自带的 + 播放时的符号链接），
#          详见项目 docs/13 §六，不要理解成"零 flash 写入"。
# ============================================================================

SD=/home/hd1                 # SD 挂载点（init.sh 里已挂好）
L=$SD/hack                   # SD 上的工作目录
W=/tmp/hack                  # 内存盘工作目录（避免依赖 SD 的文件句柄）

mkdir -p $L $W 2>/dev/null
LOG=$W/boot.log

{
  echo "==================== equip_test.sh v5 ===================="
  echo "date      = $(date)"
  echo "argv0     = $0"
  echo "cwd       = $(pwd)"
  echo "sd        = $SD"
  echo "id        = $(id)"
  echo "uname     = $(uname -a)"
  echo "version   = $(grep -E '^version=' /home/version 2>/dev/null | head -1)"

  # ---- 1. 环回口 ----------------------------------------------------------
  # 原厂由 S80network 在 init.sh 之后才配 lo；init.sh 被截断后走不到那里，
  # 这里提前补上（telnetd / wpa_supplicant 的控制 socket 都可能需要）。
  ifconfig lo 127.0.0.1 up && echo "lo: UP"

  # ---- 2. 开 root shell（调试用）-------------------------------------------
  # 注意：这会允许同网段任何人以 root 登录。用完建议拔卡或改 /etc/inittab。
  telnetd -l /bin/sh && echo "telnetd: OK"

  # ---- 3. 还原"正常启动" ------------------------------------------------
  # 两条路径：
  #   a) 固件版本与预设一致 → 直接用 SD 上的 init_norm.sh。
  #      它不只是裁剪版，还包含三处针对语音助手的关键改动：
  #        - rmm 启动守护（rmm 没起来就恢复原厂 rmm 并重启，防变砖）
  #        - 停录像（record_event / mp4record，省 CPU 和内存）
  #        - 停云服务（cloud，省约 0.9MB，避免和音频抢内存）
  #   b) 版本不同 → 现场从本机 /home/init.sh 裁剪，再追加 autostart.sh。
  #      这样能跑起来，但没有上述三项优化。
  FWV=$(grep -E '^version=' /home/version 2>/dev/null | head -1 | cut -d= -f2)
  echo "固件版本 = ${FWV:-未知}"
  case "$FWV" in
    1.8.7.0C*)
      cp -f $L/init_norm.sh $W/init_norm.sh 2>>$LOG
      echo "init_norm.sh = $(sed -n '$=' $W/init_norm.sh 2>/dev/null) 行（预设，含 rmm 守护/停录像/停云服务）"
      ;;
    *)
      N=$(grep -n 'test/equip_test.sh' /home/init.sh 2>/dev/null | head -1 | cut -d: -f1)
      echo "init.sh 钩子行 = ${N:-未找到}（固件不匹配，走现场裁剪）"
      if [ -n "$N" ]; then
        sed "1,$((N+3))d" /home/init.sh > $W/init_norm.sh 2>>$LOG
        echo "init_norm.sh = $(sed -n '$=' $W/init_norm.sh 2>/dev/null) 行（现场裁剪）"
        # 补上「停录像/停云服务」这两项优化；rmm 守护依赖 rmm_orig，仅在预设包里可靠
        sed -i 's|^[[:space:]]*\./record_event &|# ./record_event \&|' $W/init_norm.sh 2>/dev/null
        sed -i 's|^[[:space:]]*\./mp4record 60 &|# ./mp4record 60 \&|' $W/init_norm.sh 2>/dev/null
        sed -i 's|^[[:space:]]*\./cloud &|# ./cloud \&|' $W/init_norm.sh 2>/dev/null
        # rmm 换成 SD 上的 patch 版（原厂有 19 段播放上限，长回答会卡顿），
        # 并去掉「把 /home/rmm 改成符号链接」的兜底 —— 那样拔卡就断链了
        if [ -f $L/bin/rmm ]; then
          sed -i 's|^[[:space:]]*\./rmm &|/tmp/hd1/hack/bin/rmm \&|' $W/init_norm.sh 2>/dev/null
          sed -i '/ln -sf .*rmm_orig .*\/home\/rmm/d' $W/init_norm.sh 2>/dev/null
          echo "  rmm 已指向 SD 上的 patch 版"
        fi
        echo "  已停用 录像/云服务（现场补丁）"
      elif [ -s $L/init_norm.sh ]; then
        cp -f $L/init_norm.sh $W/init_norm.sh 2>>$LOG
        echo "!! 现场裁剪失败，回退到 SD 预设（$(sed -n '$=' $W/init_norm.sh 2>/dev/null) 行）"
      fi
      ;;
  esac
  if [ -s $W/init_norm.sh ] && [ -f $L/autostart.sh ]; then
    # 预设版已内置自启段，避免重复追加
    if ! grep -q 'xiaozhi autostart' $W/init_norm.sh 2>/dev/null; then
      cat $L/autostart.sh >> $W/init_norm.sh
      echo "  + autostart.sh = $(sed -n '$=' $W/init_norm.sh 2>/dev/null) 行"
    else
      echo "  自启段已内置，跳过追加"
    fi
  fi

  # ---- 3.5 rmm 检查（不替换 /home/rmm）-----------------------------------
  # 原厂 rmm 的 rmm_msg_handle 有一道闸门：
  #     0x380d4  cmp  lr, #19         ; 连续播放次数
  #     0x380dc  bgt  0x380f0         ; >19 就跳过 send_ao_beep
  # 我们的 TTS 每 640ms 触发一次 mq，长回答必然超过 19 次，之后就不再播放，
  # 听感是"播到后面卡顿/丢句"。patch 版把那处 bgt 改成了 nop。
  #
  # 做法是让 init_norm.sh 直接启动 SD 上的 patch 版（文件名就叫 rmm，
  # 这样 pidof rmm 仍然有效），**不改 /home/rmm** —— 改成符号链接的话，
  # 拔卡或卡坏时链接断掉，原厂流程起不来 rmm，相机等于变砖。
  if [ -f $L/bin/rmm ]; then
    echo "rmm: 将使用 SD 上的 patch 版（$(ls -l $L/bin/rmm | awk '{print $5}') 字节）"
  else
    echo "rmm: !! SD 上没有 patch 版，沿用原厂（长回答会卡顿）"
  fi
  echo "========================================================="
} >> $LOG 2>&1

# ---- 4. 后台守望：启动完成后把日志与诊断落到 SD 卡 --------------------------
(
  sleep 150
  {
    echo "=== post-boot diag $(date) ==="
    echo "--- ifconfig ---" ; ifconfig
    echo "--- ps -w ----"   ; ps -w
    echo "--- netstat ---"   ; netstat -an
    echo "--- mqueue ----"   ; ls -l /dev/mqueue
    echo "--- wpa -------"   ; ls -l /home/wpa_supplicant.conf /etc/wpa_supplicant.conf
    echo "--- version ---"   ; cat /home/version
    echo "--- mtd ------ "   ; cat /proc/mtd
    echo "=== end diag ==="
  } > $W/diag.txt 2>&1
  cp -f $W/boot.log $W/init_norm.log $W/diag.txt /tmp/xz.log $L/ 2>/dev/null
  sync
) >/dev/null 2>&1 &

# ---- 5. 交还控制权 --------------------------------------------------------
# 用 exec 替换本进程，跑完即结束，行为与"没插卡时的 init.sh"完全一致。
if [ -s $W/init_norm.sh ]; then
  exec /bin/sh $W/init_norm.sh >> $W/init_norm.log 2>&1
else
  echo "没有可用的 init_norm.sh，放弃交还（相机将停在无应用层状态）" >> $LOG
  echo "此时 telnet 仍可用，可手工排查：telnet <相机IP>" >> $LOG
fi
