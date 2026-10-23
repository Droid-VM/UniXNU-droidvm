#!/usr/bin/env bash
# Run a command in the iPadOS guest as root (an interactive zsh without one), over the guest's dropbear (an ownervisor service on
# port 2222). The guest's address is reachable only from the phone (the app's pbridge), so the connection rides `nc` there.
#   guest-ssh.sh --key        make the key once (GUEST_KEY, default out/ssh/id_ed25519) and put its .pub into the guest's
#                             /private/var/ngpu/ssh/authorized_keys; needed after every new Data seed (the file lives on Data)
#   guest-ssh.sh [COMMAND]    GUEST_IP is the guest's IPv4 (health.py's network line); GSSH_TIMEOUT bounds the run (default 90 s)
# stdin passes through, so `guest-ssh.sh "toybox cat > /private/var/ngpu/bin/x.new" < x` copies a file in.
set -u
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
KEY=${GUEST_KEY:-$RIG/out/ssh/id_ed25519}
if [ "${1:-}" = --key ]; then
  [ -f "$KEY" ] || { mkdir -p "$(dirname "$KEY")" && ssh-keygen -q -t ed25519 -N '' -f "$KEY"; }
  exec python3 "$RIG/export.py" put "$KEY.pub" /private/var/ngpu/ssh/authorized_keys 0644
fi
: "${PHONE:?set PHONE to the adb serial of the phone}"
: "${GUEST_IP:?set GUEST_IP to the guest IPv4 (health.py prints it)}"
NC=${PHONE_NC:-/data/adb/ksu/bin/busybox nc}
exec timeout "${GSSH_TIMEOUT:-90}" ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes \
  -o ConnectTimeout=25 -o LogLevel=ERROR -o "ProxyCommand=adb -s $PHONE shell su -c '$NC $GUEST_IP 2222'" root@guest "$@"
