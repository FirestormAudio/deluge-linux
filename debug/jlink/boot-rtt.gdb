set pagination off
set confirm off
set height 0
set width 0
set architecture arm
target remote :2331
monitor reset
monitor cp15 1, 0, 0, 0 = 0x00C50078
monitor cp15 12, 0, 0, 0 = 0x0C000000
set $cpsr = 0x1D3
load out/u-boot
monitor exec SetRTTAddr 0x20080000
python
gdb.execute("continue")
import time
time.sleep(20)
end
