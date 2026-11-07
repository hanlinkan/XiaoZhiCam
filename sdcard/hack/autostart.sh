# ---- xiaozhi autostart ----
# 小智语音助手（p8s 版）：等 ra0 拿到 IP 后常驻，崩了 5 秒重启。
# 日志超过 512KB 就轮转一次，避免把 tmpfs 写满。
if [ -x /tmp/hd1/hack/bin/p8s ] && [ -f /tmp/hd1/hack/launch_p8.sh ]; then
    (
        n=0
        while [ $n -lt 60 ]; do
            if ifconfig ra0 2>/dev/null | grep -q "inet addr"; then
                break
            fi
            n=$((n + 1))
            sleep 1
        done
        echo "xiaozhi: network wait ${n}s" >> /tmp/xz.log

        cd /tmp/hd1/hack || exit 0
        while :; do
            sz=$(wc -c < /tmp/xz.log 2>/dev/null)
            if [ -n "$sz" ] && [ "$sz" -gt 524288 ]; then
                mv -f /tmp/xz.log /tmp/xz.old
            fi
            echo "=== p8s start $(date) ===" >> /tmp/xz.log
            if [ -f /tmp/hd1/hack/solo16k.sh ]; then
                sh /tmp/hd1/hack/solo16k.sh >> /tmp/xz.log 2>&1
            else
                sh /tmp/hd1/hack/launch_p8.sh >> /tmp/xz.log 2>&1
            fi
            echo "=== p8s exit rc=$? $(date) ===" >> /tmp/xz.log
            sleep 5
        done
    ) &
fi
# ---- xiaozhi autostart end ----

# ---- xiaozhi guard ----
# 取代原厂 watch_process（卡模式下已停用，见 init_norm.sh）。
#
# 为什么必须自己接管：原厂 /home/wp_cmd 里挂着 mini_httpd / rmm / p2pserver /
# goolink / mp4record / cloud / record_event / p2p_tnp / oss 九项「缺了就执行」
# 的规则，watch_process 每 10s 扫一次。本机 MemTotal 只有 34MB、空闲 1~2MB，
# cloud 一项就要 ~0.9MB，被拉起来语音必崩。我们自己精简过 wp_cmd 的样机看不出
# 问题，但**没改过的相机一定会**，所以卡上一律不启动 watch_process。
#
# 这里补回它真正有价值的两件事：
#   1) rmm 掉了就重启整机（等价 wp_cmd 里 process:[rmm] cmd:[reboot]）
#   2) 持续清掉云/P2P/录像进程，把内存留给语音
(
    while :; do
        sleep 30
        if [ ! -f /tmp/hd1/hack/.norrm ] && [ ! -f /tmp/hd1/hack/.solo16k ] && ! pidof rmm >/dev/null 2>&1; then
            echo "guard: rmm 不存在，重启 $(date)" >> /tmp/xz.log
            sync
            reboot
        fi
        for p in cloud cloudAPI p2p_tnp goolink mp4record record_event p2pserver; do
            killall $p 2>/dev/null
        done
    done
) &
# ---- xiaozhi guard end ----
