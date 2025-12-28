#!/usr/bin/env python3
"""
Minimal UV-K5 Firmware Flasher
Only errors and progress output
Max firmware size: 120 KB
"""

import serial
import serial.tools.list_ports
import time
import sys
import struct

# Constants
BAUDRATE = 38400
TIMEOUT = 5
MAX_FIRMWARE_SIZE = 120 * 1024  # 120 KB

# Message types
MSG_NOTIFY_DEV_INFO = 0x0518
MSG_NOTIFY_BL_VER = 0x0530
MSG_PROG_FW = 0x0519
MSG_PROG_FW_RESP = 0x051A

# Obfuscation table
OBFUS_TBL = bytes([
    0x16, 0x6c, 0x14, 0xe6, 0x2e, 0x91, 0x0d, 0x40,
    0x21, 0x35, 0xd5, 0x40, 0x13, 0x03, 0xe9, 0x80
])


class UVK5Flasher:
    def __init__(self, port: str):
        self.port = port
        self.ser = None
        self.read_buffer = bytearray()

    def connect(self) -> bool:
        try:
            self.ser = serial.Serial(port=self.port, baudrate=BAUDRATE, timeout=TIMEOUT, write_timeout=TIMEOUT)
            time.sleep(0.5)
            return True
        except Exception as e:
            print(f"Error opening port {self.port}: {e}")
            return False

    def disconnect(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def obfuscate(self, data: bytearray, offset: int, size: int):
        for i in range(size):
            data[offset + i] ^= OBFUS_TBL[i % len(OBFUS_TBL)]

    def calc_crc(self, data: bytes, offset: int, size: int) -> int:
        crc = 0
        for i in range(size):
            b = data[offset + i] & 0xFF
            crc ^= b << 8
            for _ in range(8):
                if crc & 0x8000:
                    crc = (crc << 1 ^ 0x1021) & 0xFFFF
                else:
                    crc = (crc << 1) & 0xFFFF
        return crc

    def make_packet(self, msg: bytes) -> bytearray:
        msg_len = len(msg)
        if msg_len % 2 != 0:
            msg_len += 1

        buf = bytearray(8 + msg_len)
        struct.pack_into('<H', buf, 0, 0xCDAB)
        struct.pack_into('<H', buf, 2, msg_len)
        buf[4:4+len(msg)] = msg

        crc = self.calc_crc(buf, 4, msg_len)
        struct.pack_into('<H', buf, 4 + msg_len, crc)
        struct.pack_into('<H', buf, 6 + msg_len, 0xBADC)

        self.obfuscate(buf, 4, 2 + msg_len)
        return buf

    def send_message(self, msg: bytes):
        packet = self.make_packet(msg)
        self.ser.write(packet)
        self.ser.flush()

    def fetch_message(self) -> dict | None:
        if len(self.read_buffer) < 8:
            return None

        pack_begin = -1
        for i in range(len(self.read_buffer) - 1):
            if self.read_buffer[i] == 0xAB and self.read_buffer[i+1] == 0xCD:
                pack_begin = i
                break

        if pack_begin == -1:
            if len(self.read_buffer) and self.read_buffer[-1] == 0xAB:
                self.read_buffer = self.read_buffer[-1:]
            else:
                self.read_buffer.clear()
            return None

        if len(self.read_buffer) - pack_begin < 8:
            return None

        msg_len = struct.unpack_from('<H', self.read_buffer, pack_begin + 2)[0]
        pack_end = pack_begin + 6 + msg_len

        if len(self.read_buffer) < pack_end + 2:
            return None

        if self.read_buffer[pack_end] != 0xDC or self.read_buffer[pack_end+1] != 0xBA:
            del self.read_buffer[:pack_begin + 2]
            return None

        msg_buf = bytearray(self.read_buffer[pack_begin+4:pack_begin+4+msg_len+2])
        self.obfuscate(msg_buf, 0, msg_len + 2)

        msg_type = struct.unpack_from('<H', msg_buf, 0)[0]
        data = bytes(msg_buf[4:])

        del self.read_buffer[:pack_end + 2]
        return {'msg_type': msg_type, 'data': data}

    def wait_for_device_info(self):
        timeout = 500
        acc = 0
        last_ts = 0

        while timeout > 0:
            time.sleep(0.01)
            timeout -= 1

            if self.ser.in_waiting:
                self.read_buffer.extend(self.ser.read(self.ser.in_waiting))

            msg = self.fetch_message()
            if not msg or msg['msg_type'] != MSG_NOTIFY_DEV_INFO:
                continue

            now = time.time()
            dt = (now - last_ts) * 1000 if last_ts else 0
            last_ts = now

            if 5 <= dt <= 1000:
                acc += 1
                if acc >= 5:
                    uid = msg['data'][:16]
                    bl_ver_end = msg['data'][16:32].find(b'\x00')
                    bl_version = msg['data'][16:16 + (bl_ver_end if bl_ver_end != -1 else 16)].decode('ascii', errors='ignore')
                    return uid, bl_version
            else:
                acc = 0

        raise TimeoutError("Radio not detected")

    def perform_handshake(self, bl_version: str):
        for _ in range(3):
            time.sleep(0.05)
            if self.ser.in_waiting:
                self.read_buffer.extend(self.ser.read(self.ser.in_waiting))

            msg = self.fetch_message()
            if msg and msg['msg_type'] == MSG_NOTIFY_DEV_INFO:
                bl_msg = bytearray(8)
                struct.pack_into('<HH', bl_msg, 0, MSG_NOTIFY_BL_VER, 4)
                bl_msg[4:8] = bl_version[:4].encode('ascii')
                self.send_message(bl_msg)

        time.sleep(0.2)
        while self.fetch_message():
            pass  # clear buffer

    def program_firmware(self, firmware_data: bytes):
        page_count = (len(firmware_data) + 255) // 256
        timestamp = int(time.time() * 1000) & 0xFFFFFFFF

        page_index = 0
        retry = 0
        while page_index < page_count:
            print(f"Progress: {page_index / page_count * 100:5.1f}% ({page_index+1}/{page_count})", end='\r')

            msg = bytearray(272)
            struct.pack_into('<HH', msg, 0, MSG_PROG_FW, 268)
            struct.pack_into('<I', msg, 4, timestamp)
            struct.pack_into('<HH', msg, 8, page_index, page_count)

            offset = page_index * 256
            length = min(256, len(firmware_data) - offset)
            msg[16:16+length] = firmware_data[offset:offset+length]

            self.send_message(msg)

            responded = False
            for _ in range(300):
                time.sleep(0.01)
                if self.ser.in_waiting:
                    self.read_buffer.extend(self.ser.read(self.ser.in_waiting))

                resp = self.fetch_message()
                if not resp:
                    continue
                if resp['msg_type'] == MSG_PROG_FW_RESP:
                    resp_page = struct.unpack_from('<H', resp['data'], 4)[0]
                    err = struct.unpack_from('<H', resp['data'], 6)[0]
                    if resp_page != page_index:
                        continue
                    if err != 0:
                        print(f"\nError writing page {page_index}: code {err}")
                        retry += 1
                        if retry > 3:
                            raise RuntimeError(f"Too many errors on page {page_index}")
                        break
                    responded = True
                    retry = 0
                    break

            if responded:
                page_index += 1
            else:
                print(f"\nTimeout on page {page_index}")
                retry += 1
                if retry > 3:
                    raise RuntimeError(f"Too many timeouts on page {page_index}")

        print(f"\nProgress: 100.0% ({page_count}/{page_count})")

    def flash(self, firmware_path: str):
        # Read and check size
        try:
            with open(firmware_path, 'rb') as f:
                data = f.read()
            if len(data) > MAX_FIRMWARE_SIZE:
                print(f"Error: firmware size {len(data)} bytes exceeds limit of {MAX_FIRMWARE_SIZE} bytes")
                return False
        except Exception as e:
            print(f"Error reading file {firmware_path}: {e}")
            return False

        if not self.connect():
            return False

        try:
            self.read_buffer.clear()
            time.sleep(1)

            _, bl_version = self.wait_for_device_info()

            self.perform_handshake(bl_version)
            self.program_firmware(data)

            print("Flashing completed successfully")
            return True

        except Exception as e:
            print(f"Flashing error: {e}")
            return False
        finally:
            self.disconnect()


def main():
    import argparse
    parser = argparse.ArgumentParser(description='Minimal UV-K5 firmware flasher')
    parser.add_argument('firmware', help='Path to firmware file (.bin)')
    parser.add_argument('-p', '--port', help='Serial port, e.g. COM3 or /dev/ttyUSB0')
    parser.add_argument('-l', '--list', action='store_true', help='List available ports')

    args = parser.parse_args()

    if args.list:
        ports = serial.tools.list_ports.comports()
        if not ports:
            print("No ports found")
        else:
            print("Available ports:")
            for p in ports:
                print(f"  {p.device} - {p.description}")
        return

    port = args.port
    if not port:
        ports = serial.tools.list_ports.comports()
        if not ports:
            print("Error: no ports found, specify manually with -p")
            sys.exit(1)
        print("Available ports:")
        for i, p in enumerate(ports, 1):
            print(f"  {i}. {p.device} - {p.description}")
        port = input("Enter port name: ").strip()
        if not port:
            print("Error: port not specified")
            sys.exit(1)

    flasher = UVK5Flasher(port)
    success = flasher.flash(args.firmware)
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
