#!/bin/bash
# netup.sh - on a Mac Pro running macOS 27, configd alone can leave a wired port without an address. For up to six
# minutes after start this asks DHCP once for every Ethernet port that has link and no IPv4 address, then exits.
PATH=/usr/bin:/bin:/usr/sbin:/sbin
end=$(( $(date +%s) + 360 ))
while [ "$(date +%s)" -lt "$end" ]; do
  for p in $(networksetup -listallhardwareports 2>/dev/null | awk '/^Hardware Port: .*Ethernet/{e=1; next} e && /^Device:/{print $2; e=0}'); do
    ifconfig "$p" 2>/dev/null | grep -q 'status: active' || continue
    ipconfig getifaddr "$p" >/dev/null 2>&1 && continue
    done_file=/var/run/nullmoth-netup-$p; [ -e "$done_file" ] && continue
    ipconfig set "$p" DHCP && : > "$done_file" && logger -t nullmoth-netup "asked DHCP on $p"
  done
  sleep 10
done
