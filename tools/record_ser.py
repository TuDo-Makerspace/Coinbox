import time
import serial
import sys

SERIAL_PORT = "/dev/ttyUSB0"
BAUD_RATE = 115200
DURATION = 20
OUT_FILE = "adc_readings.csv"


def record_serial_to_file(port, baud, duration, filename):
    try:
        ser = serial.Serial(port, baud, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening {port}: {e}", file=sys.stderr)
        sys.exit(1)

    start_time = time.time()
    with open(filename, "w") as f:
        f.write("time_s,value\n")
        while time.time() - start_time < duration:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if not line:
                continue
            try:
                val = int(line)
            except ValueError:
                # skip non-integer lines
                continue

            timestamp = time.time() - start_time
            f.write(f"{timestamp:.3f},{val}\n")

    ser.close()
    print(f"Finished recording {duration}s → {filename}")


if __name__ == "__main__":
    record_serial_to_file(SERIAL_PORT, BAUD_RATE, DURATION, OUT_FILE)
