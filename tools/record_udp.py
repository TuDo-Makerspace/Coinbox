#!/usr/bin/env python3
import argparse
import csv
import socket
import time
import sys
import select

DEFAULT_IP = "192.168.0.31"  # ESP32 address
DEVICE_PORT = 12345  # Port the ESP32 listens on
LOCAL_PORT = 40000  # Our source/listen port (must match the one we send from)
DEFAULT_DURATION = 20
DEFAULT_OUT = "adc_readings.csv"
PING_PAYLOAD = b"ping\n"


def record_udp(ip, dev_port, local_port, duration, outfile):
    # Open one socket for both send + receive so source port = LOCAL_PORT
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", local_port))
    sock.setblocking(False)

    # Kick the ESP so it learns our (IP, port)
    sock.sendto(PING_PAYLOAD, (ip, dev_port))

    start = time.time()
    end = start + duration

    with open(outfile, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["time_s", "value"])

        buf = b""
        poller = select.poll()
        poller.register(sock, select.POLLIN)

        while True:
            now = time.time()
            if now >= end:
                break

            # Wait at most remaining time (converted to ms)
            remaining_ms = int(max(0, (end - now) * 1000))
            events = poller.poll(remaining_ms)
            if not events:
                continue

            data, _ = sock.recvfrom(4096)
            buf += data

            # Lines are separated by '\n'
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    val = int(line)
                except ValueError:
                    continue
                writer.writerow([f"{time.time() - start:.3f}", val])

    sock.close()
    print(f"Finished recording {duration}s → {outfile}")


def main():
    ap = argparse.ArgumentParser(description="Record UDP ADC values from ESP32.")
    ap.add_argument("-i", "--ip", default=DEFAULT_IP, help="ESP32 IP address")
    ap.add_argument(
        "-d", "--devport", default=DEVICE_PORT, type=int, help="ESP32 listen port"
    )
    ap.add_argument(
        "-l",
        "--localport",
        default=LOCAL_PORT,
        type=int,
        help="local UDP source/listen port",
    )
    ap.add_argument(
        "-t", "--time", default=DEFAULT_DURATION, type=float, help="duration (s)"
    )
    ap.add_argument("-o", "--out", default=DEFAULT_OUT, help="output CSV file")
    args = ap.parse_args()

    try:
        record_udp(args.ip, args.devport, args.localport, args.time, args.out)
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)


if __name__ == "__main__":
    main()
