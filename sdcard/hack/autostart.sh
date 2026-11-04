# ---- xiaozhi autostart ----
# 小智语音助手（p8s 版，2026-10-31）：等 ra0 拿到 IP 后常驻，崩了 5 秒重启。
# 旧固件 xiaozhi-cam 已废弃并删除；launch.sh 的 rmm 提权逻辑移入 launch_p8.sh。
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
            sh /tmp/hd1/hack/launch_p8.sh >> /tmp/xz.log 2>&1
            echo "=== p8s exit rc=$? $(date) ===" >> /tmp/xz.log
            sleep 5
        done
    ) &
fi
# ---- xiaozhi autostart end ----

# ---- xiaozhi quiet ----
# 清掉开机时被 dispatch / oss 拉起来的云服务（语音助手用不上，省 ~2MB）。
# 分三轮，因为有的进程起得晚。
(
    for wait in 20 25 25; do
        sleep $wait
        for p in cloud cloudAPI p2p_tnp goolink; do
            killall $p 2>/dev/null
        done
    done
    echo "xiaozhi: quiet pass done $(date)" >> /tmp/xz.log
) &
# ---- xiaozhi quiet end ----
