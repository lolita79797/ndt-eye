import serial
import time

ser = serial.Serial('COM7', 115200, timeout=1)

start = time.time()
while time.time() - start < 15:
    if ser.in_waiting:
        line = ser.readline()
        print(line.decode('utf-8', errors='replace'), end='')

ser.close()
