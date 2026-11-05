# Display-only checkpoint. No file load, raw memory access, bootm, go or reset.
# echoe uses the pinned firmware's existing stderr video route.
echoe "IOSWITCH D0: DISPLAY SCRIPT ENTERED"
# lcdputs addresses the video console directly, independently of stdout.
setcurs 0 0
lcdputs "IOSWITCH D1: DIRECT VIDEO - CPU PROBE NOT LAUNCHED"
setcurs 0 32
lcdputs "HOLD POWER AT LEAST 20 SECONDS TO TURN OFF"
# Repaint the same cells so a changing A/B proves the interpreter is alive.
while true; do
    setcurs 0 64
    lcdputs "IOSWITCH D2: SCRIPT HEARTBEAT A"
    sleep 1
    setcurs 0 64
    lcdputs "IOSWITCH D2: SCRIPT HEARTBEAT B"
    sleep 1
done
