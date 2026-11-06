set pagination off
set confirm off
set architecture arm
target remote :2331
monitor reset
monitor cp15 1, 0, 0, 0 = 0x00C50078
monitor cp15 12, 0, 0, 0 = 0x0C000000
set $cpsr = 0x1D3
python
def test(base, n):
    errs=0
    for i in range(n):
        a=(base+i*4)&0xffffffff
        gdb.execute("set {unsigned int}0x%x = 0x%x"%(a,a))
    for i in range(n):
        a=(base+i*4)&0xffffffff
        v=int(gdb.parse_and_eval("*(unsigned int*)0x%x"%a))&0xffffffff
        if v!=a:
            errs+=1
            if errs<=6: print("  MISMATCH @0x%08x wrote 0x%08x read 0x%08x"%(a,a,v))
    print("region 0x%08x: %d words, %d errors"%(base,n,errs))
for b in (0x0C400000, 0x0E000000, 0x0F800000):
    test(b, 256)
end
detach
quit
