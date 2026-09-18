import subprocess
import time

gdb_cmd = [
    'xtensa-esp32s3-elf-gdb',
    '-q',
    'build/ndt_eye_firmware.elf'
]

p = subprocess.Popen(gdb_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)

def send(cmd):
    print(f"(gdb) {cmd}")
    p.stdin.write(cmd + "\n")
    p.stdin.flush()
    time.sleep(0.8)

send("target remote localhost:3333")
send("break lcd_driver_draw_frame")
send("break on_lcd_trans_done")
send("info breakpoints")
send("continue")
send("print *(uint32_t*)((char*)s_dma_done_sem + 12)")
send("continue")
send("print *(uint32_t*)((char*)s_dma_done_sem + 12)")
send("detach")
send("quit")

out, _ = p.communicate(timeout=12)
print(out)
