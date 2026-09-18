import subprocess
import time

gdb_cmd = [
    'xtensa-esp32s3-elf-gdb',
    '-q',
    'D:/Espressif/esp-idf-5.5.2/Repo/ndt-eye/build/ndt_eye_firmware.elf'
]

p = subprocess.Popen(gdb_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)

def send(cmd):
    print(f"(gdb) {cmd}")
    p.stdin.write(cmd + "\n")
    p.stdin.flush()
    time.sleep(1.0)

send("target remote localhost:3333")
send("info threads")
send("thread apply all bt")
send("detach")
send("quit")

out, _ = p.communicate(timeout=15)
print(out)
