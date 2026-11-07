
if [ -f "/home/hd1/wpa_supplicant.conf1" ]; then
	cp /home/ui.conf_bak /etc/ui.conf
fi

export LD_LIBRARY_PATH=/home/libusr:$LD_LIBRARY_PATH
mv /home/default.script /usr/share/udhcpc -f

rm /etc/resolv.conf
ln -s /tmp/resolv.conf /etc/resolv.conf

/home/log_server &

cp /etc/wpa_supplicant.conf /home
mv /home/hd1/wpa_supplicant.conf /home
/home/crypt_file -d /home/hd1/crypt.bin /home/wpa_supplicant.conf
rm /home/hd1/crypt.bin
cp /home/hd1/wpa_supplicant.conf1 /home/wpa_supplicant.conf

cd /home
mount |grep "/tmp"
/home/productioninfoget.sh
insmod cpld_periph.ko

cd /home/3518
./load3518_audio -i

if [ -f "/home/notfirst" ]; then 
if [ -f "/home/wpa_supplicant.conf" ]; then  
echo " "
else 
/home/rmm "/home/welcome.g726" 1
/home/rmm "/home/start_wait.g726" 1 
himm 0x20050074 0x06802424
fi
/home/checkdisk 
rm /home/do_format
else
if [ -f "/home/wpa_supplicant.conf" ]; then  
/home/rmm "/home/welcome.g726" 1
/home/rmm "/home/start_wait.g726" 1    
himm 0x20050074 0x06802424
fi
fi

/home/led_ctl -boff -yon &

bcmver="bd1e"
bcmver1="0bdc"
bcmcmd=$(lsusb|grep "0a5c"|cut -d':' -f3)

if [ $bcmver = $bcmcmd ];then
	echo 1 > /tmp/isbcm
	#/home/bcm/nvram set /home/bcm/nvram.bin
	/home/bcm/nvram get
	mount -t usbfs none /proc/bus/usb
	/home/bcm/bcmdl -n /tmp/nvram.bin /home/bcm/fw_bcmdhd_xy_r2.bin.trx -C 5 
	insmod /home/bcm/bcmdhd.ko iface_name=ra0
	rm -f /home/mtprealloc7601Usta.ko
	rm -f /home/mt7601Usta.ko
	echo "BCM 43143"
elif [ $bcmver1 = $bcmcmd ];then
	echo 1 > /tmp/isbcm
	#/home/bcm/nvram set /home/bcm/nvram.bin
	/home/bcm/nvram get
	mount -t usbfs none /proc/bus/usb
	/home/bcm/bcmdl -n /tmp/nvram.bin /home/bcm/fw_bcmdhd_xy_r2.bin.trx -C 5 
	insmod /home/bcm/bcmdhd.ko iface_name=ra0
	rm -f /home/mtprealloc7601Usta.ko
	rm -f /home/mt7601Usta.ko
	echo "BCM 43143"
else
	echo 1 > /tmp/ismtk
	insmod /home/mtprealloc7601Usta.ko
	insmod /home/mt7601Usta.ko
	rm -rf /home/bcm/
	echo "MTK 7601"
fi

ifconfig ra0 up

#if [ -f "/home/need_update" ]; then
#	/home/script/wificonn.sh 1
#	if [ -f "/tmp/gw1" ]; then
#	/home/need_update
#	fi
#fi

sysctl -w fs.mqueue.msg_max=256
mkdir /dev/mqueue
mount -t mqueue none /dev/mqueue

#insmod /home/cpld_wdg.ko
#insmod /home/cpld_periph.ko
#insmod /home/iap_auth.ko
/home/gethwplatform

#now begin app
   sysctl -w net.ipv4.tcp_mem='3072    4096    2000000' 
   sysctl -w net.core.wmem_max='2000000'
   sysctl -w net.ipv4.tcp_keepalive_time=300 net.ipv4.tcp_keepalive_intvl=6 net.ipv4.tcp_keepalive_probes=3 
         
   insmod /home/as-iosched.ko
   echo "anticipatory" > /sys/block/mmcblk0/queue/scheduler 
   echo "1024" > /sys/block/mmcblk0/queue/read_ahead_kb   
   
   umount /home/hd1
   umount /home/hd2
   
   mount -t vfat /dev/hd1 /home/hd1
   mkdir /home/hd1/record
   mkdir /home/hd1/record_sub
   mount -t vfat /dev/hd2 /home/hd2
   mkdir /home/hd2/record_sub
   rm /home/web/sd/* -rf
   #mount -t vfat /dev/hd1 /home/web/sd
   
cd /home/3518
./load3518_left -i
/home/detect_ve
himm 0x20050074 0x06802424
   
cd /home                  
	./peripheral &   
	./dispatch &
	./exnet &
	#./mysystem &
	
count=5

while [ $count -gt 0 ]
do
if [ -f "/tmp/init_finish" ]; then         
        break
else
        count=`expr $count - 1`
        echo "wait init" $count
        sleep 1
fi
done
	
if [ -f "/home/notfirst" ]; then         
if [ -f "/home/wpa_supplicant.conf" ]; then         
echo "wifi connectting"
/home/monitor_wifi &
cd /home
else                              
echo "1" > /tmp/waitdump
echo "1" > /tmp/announce_success  
/home/monitor_dump && rm /tmp/waitdump && /home/monitor_wifi &                 
cd /home
fi 
else
echo "first start"
fi 

cd /home  
# 从 SD 卡启动 patch 版 rmm（文件名保持 rmm，pidof rmm 仍然有效）。
# 原厂 rmm 的 rmm_msg_handle 有「连续播放 19 次就跳过」的闸门：我们的 TTS
# 每 640ms 触发一次 mq，长回答必然超过，听感是播到后面卡顿/丢句。
# patch 版把那处 bgt 改成了 nop（另有 1 处时间常量 1000000 -> 20000）。
#
# v6：**卡上文件缺失/损坏时退回 flash 上的原厂 rmm，绝不 reboot 循环。**
# 没改过 flash 的相机 /home/rmm 一定是原版实体文件，天然兜底；本机这种被改过的
# 样机如果 /home/rmm 是断链，兜底也会失败，此时保持存活让 telnet 能进去修，
# 而不是无限重启。
#
# v8：调试开关 —— 卡上存在 .norrm 时跳过 rmm（用于验证「无 rmm」模式）。
#     注意：rmm 从未启动时 HI_MPI_SYS_Init 会失败（0xA0028010 NOTREADY），
#     p8s 起不来。只有先启动过 rmm 再 kill -9 才有可用的 MPP 状态。
if [ -f /tmp/hd1/hack/.norrm ]; then
    echo "norrm: 跳过 rmm（调试模式，p8s 将无法 Init MPP）" >> /tmp/xz.log
elif [ -x /tmp/hd1/hack/bin/rmm ]; then
    /tmp/hd1/hack/bin/rmm &
    if [ -f /tmp/hd1/hack/.solo16k ]; then sleep 20; else sleep 6; fi
    if ! pidof rmm >/dev/null 2>&1; then
        echo "SD rmm 未起来，退回原厂 /home/rmm" >> /tmp/xz.log
        ./rmm &
    fi
else
    echo "SD 上没有 rmm，直接用原厂 /home/rmm" >> /tmp/xz.log
    ./rmm &
fi

while [ 1 -eq 1 ]
do
if [ -f "/tmp/waitdump" ]; then         
				echo "do waiting dump"                                 
        sleep 1
else
				break
fi
done
	
	#cp update /tmp
	#/tmp/update &
	cp update.sh flash_eraseall crcCheck /home/tmpfs
	
	cd /home/web                                                                 
	./mini_httpd &
	cd /home
	# ./record_event &
	# ./mp4record 60 &
#	./p2pserver &
	./rtspsvr &
	# ./cloud &   # 已停用：省 0.9MB，且会拉起 cloudAPI
# v6：卡模式不启动原厂 watch_process。
# 原厂 /home/wp_cmd 挂着 p2pserver/goolink/mp4record/cloud/record_event/p2p_tnp
# 六项「缺了就拉起」的规则，每 10s 扫一次；本机 MemTotal 仅 34MB（空闲 1~2MB），
# 拉起 cloud(~0.9MB) 等会直接把内存吃光，语音必崩。
# 我们改过 wp_cmd 的样机没这问题，但**没改过的相机有**，所以这里一律不启动，
# 由 autostart.sh 的 guard 循环接管「rmm 存活检查 + 云服务清理」。
# /home/watch_process &

if [ -f "/home/notfirst" ]; then         
echo "start ok!"	
else                              
echo "1" > /home/notfirst
echo "first start, auto reboot!"
reboot
reboot
fi 	 
	 
sleep 5

himm 0x20050068 0x327c2c
#himm 0x20050068 0x0032562c
himm 0x20050074 0x06802424
himm 0x20050078 0x18ffc001
#himm 0x20050078 0x1effc001
himm 0x20110168 0x10601
himm 0x20110188 0x10601
himm 0x20110184 0x03ff2
himm 0x20030034 0x43
himm 0x200300d0 0x1
himm 0x2003007c 0x1
himm 0x20030040 0x102
himm 0x20030040 0x202
himm 0x20030040 0x302
himm 0x20030048 0x102
himm 0x20030048 0x202
himm 0x20030048 0x302

insmod /home/hi_cipher.ko

./oss &

rm /home/hd1/FSCK*

 #end for network test
 

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
