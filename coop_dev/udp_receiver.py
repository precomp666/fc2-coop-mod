#!/usr/bin/env python3
import socket
import struct
import time
import math
import sys

UDP_IP = "0.0.0.0"
UDP_PORT = 42069
COOP_MAGIC = 0x46433243 # 'FC2C'

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Allow address reuse
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        sock.bind((UDP_IP, UDP_PORT))
    except Exception as e:
        print(f"Error binding to {UDP_IP}:{UDP_PORT} -> {e}")
        return

    print("=" * 65)
    print("      FAR CRY 2 CO-OP - UDP RECEIVER / SATELLITE RADAR      ")
    print(f" Listening for live game packets on UDP {UDP_IP}:{UDP_PORT}...")
    print(" Start Far Cry 2 via Lutris to see your live coordinates.")
    print("=" * 65)

    last_x, last_y, last_z = None, None, None
    last_time = time.time()
    packet_count = 0

    while True:
        try:
            data, addr = sock.recvfrom(1024)
            if len(data) < 28:
                continue

            magic, pkt_type, seq, timestamp, x, y, z = struct.unpack("<IIIIfff", data[:28])

            if magic != COOP_MAGIC:
                continue

            now = time.time()
            dt = now - last_time
            speed = 0.0

            if last_x is not None and dt > 0.001:
                dx = x - last_x
                dy = y - last_y
                dz = z - last_z
                dist = math.sqrt(dx*dx + dy*dy + dz*dz)
                speed = dist / dt # m/s
                speed_kmh = speed * 3.6

            last_x, last_y, last_z = x, y, z
            last_time = now
            packet_count += 1

            status = "MOVING" if speed > 0.2 else "STANDSTILL"

            sys.stdout.write(
                f"\r[RX #{seq:06d}] Addr: {addr[0]} | "
                f"X: {x:8.2f}m | Y: {y:8.2f}m | Z: {z:6.2f}m | "
                f"Speed: {speed*3.6:5.1f} km/h [{status:<10}]"
            )
            sys.stdout.flush()

        except KeyboardInterrupt:
            print("\nReceiver stopped by user.")
            break
        except Exception as e:
            print(f"\nError: {e}")
            break

if __name__ == "__main__":
    main()
