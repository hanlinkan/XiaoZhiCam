#!/bin/sh
# p8s 启动器：等 ra0 有 IP；rmm adec_sendao 提 FIFO 10；
# 日志轮转；由 init_norm.sh 的 while 监控循环调用（崩 5s 重启）
n=0
while [ $n -lt 120 ]; do
    ip=$(ifconfig ra0 2>/dev/null | grep 'inet addr' | head -1)
    [ -n "$ip" ] && break
    sleep 2; n=$((n+1))
done
# rmm 现在从 SD 卡以绝对路径启动（/tmp/hd1/hack/bin/rmm），
# 原来靠 ps 匹配 './rmm' 的方式失效，改用 pidof。
RPID=$(pidof rmm | awk '{print $1}')
if [ -n "$RPID" ]; then
    for t in /proc/$RPID/task/*; do
        [ -r "$t/comm" ] || continue
        if [ "$(cat $t/comm 2>/dev/null)" = "adec_sendao" ]; then
            chrt -f -p 10 "$(basename $t)" 2>/dev/null
        fi
    done
fi
cd /tmp/hd1/hack
exec ./bin/p8s
