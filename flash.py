#!/usr/bin/env python3
"""
UV-K5 Firmware Flasher
Python implementation for flashing firmware to Quansheng UV-K5 radio
"""

import serial
import serial.tools.list_ports
import time
import sys
import struct
from typing import Optional, Tuple

# Constants
BAUDRATE = 38400
TIMEOUT = 5

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
        self.ser: Optional[serial.Serial] = None
        self.read_buffer = bytearray()
        
    def connect(self) -> bool:
        """Connect to the radio"""
        try:
            print(f"Opening port {self.port}...")
            self.ser = serial.Serial(
                port=self.port,
                baudrate=BAUDRATE,
                timeout=TIMEOUT,
                write_timeout=TIMEOUT
            )
            time.sleep(0.5)
            print("Connected successfully")
            return True
        except Exception as e:
            print(f"Connection error: {e}")
            return False
    
    def disconnect(self):
        """Disconnect from the radio"""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("Disconnected")
    
    def obfuscate(self, data: bytearray, offset: int, size: int):
        """Apply obfuscation to data"""
        for i in range(size):
            data[offset + i] ^= OBFUS_TBL[i % len(OBFUS_TBL)]
    
    def calc_crc(self, data: bytes, offset: int, size: int) -> int:
        """Calculate CRC16"""
        crc = 0
        for i in range(size):
            b = data[offset + i] & 0xFF
            crc ^= b << 8
            for _ in range(8):
                if crc & 0x8000:
                    crc = ((crc << 1) ^ 0x1021) & 0xFFFF
                else:
                    crc = (crc << 1) & 0xFFFF
        return crc
    
    def create_message(self, msg_type: int, data_len: int) -> bytearray:
        """Create message with header"""
        msg = bytearray(4 + data_len)
        struct.pack_into('<HH', msg, 0, msg_type, data_len)
        return msg
    
    def make_packet(self, msg: bytes) -> bytearray:
        """Create packet with framing and obfuscation"""
        msg_len = len(msg)
        if msg_len % 2 != 0:
            msg_len += 1
        
        buf = bytearray(8 + msg_len)
        
        # Header
        struct.pack_into('<H', buf, 0, 0xCDAB)  # Start marker
        struct.pack_into('<H', buf, 2, msg_len)  # Length
        
        # Copy message
        buf[4:4+len(msg)] = msg
        
        # CRC
        crc = self.calc_crc(buf, 4, msg_len)
        struct.pack_into('<H', buf, 4 + msg_len, crc)
        
        # End marker
        struct.pack_into('<H', buf, 6 + msg_len, 0xBADC)
        
        # Obfuscate
        self.obfuscate(buf, 4, 2 + msg_len)
        
        return buf
    
    def send_message(self, msg: bytes):
        """Send message to radio"""
        packet = self.make_packet(msg)
        self.ser.write(packet)
        self.ser.flush()
    
    def fetch_message(self) -> Optional[dict]:
        """Parse and extract message from buffer"""
        if len(self.read_buffer) < 8:
            return None
        
        # Find packet start
        pack_begin = -1
        for i in range(len(self.read_buffer) - 1):
            if self.read_buffer[i] == 0xAB and self.read_buffer[i+1] == 0xCD:
                pack_begin = i
                break
        
        if pack_begin == -1:
            # Clean buffer
            if len(self.read_buffer) > 0 and self.read_buffer[-1] == 0xAB:
                self.read_buffer = self.read_buffer[-1:]
            else:
                self.read_buffer.clear()
            return None
        
        if len(self.read_buffer) - pack_begin < 8:
            return None
        
        # Read message length
        msg_len = struct.unpack_from('<H', self.read_buffer, pack_begin + 2)[0]
        pack_end = pack_begin + 6 + msg_len
        
        if len(self.read_buffer) < pack_end + 2:
            return None
        
        # Check end marker
        if self.read_buffer[pack_end] != 0xDC or self.read_buffer[pack_end+1] != 0xBA:
            del self.read_buffer[:pack_begin + 2]
            return None
        
        # Extract and deobfuscate
        msg_buf = bytearray(self.read_buffer[pack_begin+4:pack_begin+4+msg_len+2])
        self.obfuscate(msg_buf, 0, msg_len + 2)
        
        msg_type = struct.unpack_from('<H', msg_buf, 0)[0]
        data = bytes(msg_buf[4:])
        
        # Remove processed packet
        del self.read_buffer[:pack_end + 2]
        
        return {'msg_type': msg_type, 'data': data}
    
    def wait_for_device_info(self) -> Tuple[bytes, str]:
        """Wait for device info messages"""
        print("Waiting for device...")
        last_timestamp = 0
        acc = 0
        timeout = 0
        
        while timeout < 500:
            time.sleep(0.01)
            timeout += 1
            
            # Read available data
            if self.ser.in_waiting:
                data = self.ser.read(self.ser.in_waiting)
                self.read_buffer.extend(data)
            
            msg = self.fetch_message()
            if not msg:
                continue
            
            if msg['msg_type'] == MSG_NOTIFY_DEV_INFO:
                now = time.time()
                dt = (now - last_timestamp) * 1000 if last_timestamp > 0 else 0
                last_timestamp = now
                
                if last_timestamp > 0 and 5 <= dt <= 1000:
                    acc += 1
                    print(f"Valid message received ({acc}/5)")
                    
                    if acc >= 5:
                        uid = msg['data'][:16]
                        bl_version_end = msg['data'][16:32].find(b'\x00')
                        if bl_version_end == -1:
                            bl_version_end = 16
                        bl_version = msg['data'][16:16+bl_version_end].decode('ascii', errors='ignore')
                        print(f"Device detected! Bootloader: {bl_version}")
                        return uid, bl_version
                else:
                    acc = 0
        
        raise TimeoutError("No device detected")
    
    def perform_handshake(self, bl_version: str):
        """Perform handshake with device"""
        print("Performing handshake...")
        acc = 0
        
        while acc < 3:
            time.sleep(0.05)
            
            # Read available data
            if self.ser.in_waiting:
                data = self.ser.read(self.ser.in_waiting)
                self.read_buffer.extend(data)
            
            msg = self.fetch_message()
            if msg and msg['msg_type'] == MSG_NOTIFY_DEV_INFO:
                if acc == 0:
                    print("Sending bootloader version...")
                
                bl_msg = self.create_message(MSG_NOTIFY_BL_VER, 4)
                bl_bytes = bl_version[:4].encode('ascii')
                bl_msg[4:4+len(bl_bytes)] = bl_bytes
                self.send_message(bl_msg)
                
                acc += 1
                time.sleep(0.05)
        
        print("Waiting for handshake to complete...")
        time.sleep(0.2)
        
        # Clear buffer
        while self.read_buffer:
            msg = self.fetch_message()
            if not msg:
                break
        
        print("Handshake complete")
    
    def program_firmware(self, firmware_data: bytes):
        """Program firmware to device"""
        page_count = (len(firmware_data) + 255) // 256
        timestamp = int(time.time() * 1000) & 0xFFFFFFFF
        
        print(f"Programming {page_count} pages...")
        
        page_index = 0
        retry_count = 0
        max_retries = 3
        
        while page_index < page_count:
            progress = (page_index / page_count) * 100
            print(f"Progress: {progress:.1f}% ({page_index+1}/{page_count})", end='\r')
            
            # Create programming message
            msg = self.create_message(MSG_PROG_FW, 268)
            struct.pack_into('<I', msg, 4, timestamp)
            struct.pack_into('<H', msg, 8, page_index)
            struct.pack_into('<H', msg, 10, page_count)
            
            # Copy page data
            offset = page_index * 256
            length = min(256, len(firmware_data) - offset)
            msg[16:16+length] = firmware_data[offset:offset+length]
            
            self.send_message(msg)
            
            # Wait for response
            got_response = False
            for _ in range(300):
                time.sleep(0.01)
                
                if self.ser.in_waiting:
                    data = self.ser.read(self.ser.in_waiting)
                    self.read_buffer.extend(data)
                
                resp = self.fetch_message()
                if not resp:
                    continue
                
                if resp['msg_type'] == MSG_NOTIFY_DEV_INFO:
                    continue
                
                if resp['msg_type'] == MSG_PROG_FW_RESP:
                    resp_page_index = struct.unpack_from('<H', resp['data'], 4)[0]
                    err = struct.unpack_from('<H', resp['data'], 6)[0]
                    
                    if resp_page_index != page_index:
                        print(f"\nWarning: Expected page {page_index}, got {resp_page_index}")
                        continue
                    
                    if err != 0:
                        print(f"\nError programming page {page_index}: {err}")
                        retry_count += 1
                        if retry_count > max_retries:
                            raise RuntimeError(f"Too many errors at page {page_index}")
                        break
                    
                    got_response = True
                    retry_count = 0
                    break
            
            if got_response:
                page_index += 1
            else:
                print(f"\nTimeout on page {page_index}")
                retry_count += 1
                if retry_count > max_retries:
                    raise RuntimeError(f"Too many timeouts at page {page_index}")
        
        print(f"\nProgress: 100.0% ({page_count}/{page_count})")
        print("Programming complete!")
    
    def flash_firmware(self, firmware_path: str, expected_bl: str = ""):
        """Flash firmware file to device"""
        # Read firmware
        print(f"Reading firmware from {firmware_path}...")
        with open(firmware_path, 'rb') as f:
            firmware_data = f.read()
        print(f"Loaded {len(firmware_data)} bytes")
        
        # Connect
        if not self.connect():
            return False
        
        try:
            # Clear buffer
            self.read_buffer.clear()
            time.sleep(1)
            
            # Wait for device
            uid, bl_version = self.wait_for_device_info()
            print(f"UID: {uid.hex()}")
            
            # Check bootloader version
            if expected_bl and expected_bl != '*' and expected_bl != bl_version:
                print(f"Warning: Expected bootloader {expected_bl}, got {bl_version}")
                response = input("Continue anyway? (y/n): ")
                if response.lower() != 'y':
                    return False
            
            # Handshake
            self.perform_handshake(bl_version)
            
            # Program
            self.program_firmware(firmware_data)
            
            print("\nFlashing completed successfully!")
            return True
            
        except Exception as e:
            print(f"\nError during flashing: {e}")
            return False
        finally:
            self.disconnect()


def list_ports():
    """List available serial ports"""
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("No serial ports found")
        return
    
    print("Available ports:")
    for i, port in enumerate(ports, 1):
        print(f"  {i}. {port.device} - {port.description}")


def main():
    import argparse
    
    parser = argparse.ArgumentParser(description='UV-K5 Firmware Flasher')
    parser.add_argument('firmware', nargs='?', help='Firmware file path')
    parser.add_argument('-p', '--port', help='Serial port (e.g., COM3 or /dev/ttyUSB0)')
    parser.add_argument('-b', '--bootloader', default='', help='Expected bootloader version')
    parser.add_argument('-l', '--list', action='store_true', help='List available ports')
    
    args = parser.parse_args()
    
    if args.list:
        list_ports()
        return
    
    if not args.firmware:
        parser.print_help()
        print("\nExamples:")
        print("  python flasher.py firmware.bin -p COM3")
        print("  python flasher.py firmware.bin -p /dev/ttyUSB0 -b v2.1.3")
        return
    
    port = args.port
    if not port:
        list_ports()
        port = input("\nEnter port name: ").strip()
    
    if not port:
        print("Error: Port not specified")
        return
    
    flasher = UVK5Flasher(port)
    success = flasher.flash_firmware(args.firmware, args.bootloader)
    
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
