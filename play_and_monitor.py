import serial
import time

ser = serial.Serial('COM7', 115200, timeout=1)

print("=== Starting 60s Continuous Serial Log Monitor ===")
start = time.time()
while time.time() - start < 60:
    if ser.in_waiting:
        line = ser.readline()
        txt = line.decode('utf-8', errors='replace')
        print(txt, end='')

ser.close()
print("=== Serial Monitor Complete ===")
