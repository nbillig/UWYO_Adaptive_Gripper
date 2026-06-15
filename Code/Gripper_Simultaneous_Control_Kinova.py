#!/usr/bin/env python3
"""
Gripper I2C Controller for Kinova Gen3
Communicates with the Arduino GripperControl_v6 via the arm's Interconnect I2C.
"""

import sys
import os
import time
import struct
import threading

from kortex_api.autogen.client_stubs.DeviceManagerClientRpc import DeviceManagerClient
from kortex_api.autogen.client_stubs.InterconnectConfigClientRpc import InterconnectConfigClient
from kortex_api.autogen.messages import Common_pb2, InterconnectConfig_pb2

# Assuming utilities.py is in the parent directory as per your original script
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import utilities

# --- Constants matching the Arduino .ino ---
I2C_SLAVE_ADDR  = 0x08
I2C_TIMEOUT_MS  = 200    # ms per individual I2C read attempt
BUSY_TIMEOUT_S  = 8.0    # total seconds to wait for ready before aborting

# Host -> Arduino
I2C_CMD_START   = 0xBB
CMD_SET_POS     = 0x01
CMD_STOP_ALL    = 0x02
CMD_TARE        = 0x03
CMD_TARE_ONE    = 0x04
CMD_SET_POS_ALL = 0x05
CMD_HEARTBEAT   = 0xFF
I2C_PKT_LEN     = 9

# Arduino -> Host
I2C_STATUS_START = 0xAA
I2C_STATUS_LEN   = 27

# Gripper Constants
HOME_POS = 0

def xor_checksum(data: bytes) -> int:
    checksum = 0
    for b in data:
        checksum ^= b
    return checksum

class I2CBridge:
    def __init__(self, router):
        dm = DeviceManagerClient(router)
        self._ic = InterconnectConfigClient(router)
        self._dev_id = self._find_interconnect(dm)
        if self._dev_id is None:
            print("[ERROR] Interconnect not found.")
            sys.exit(1)
        self.lock = threading.Lock()

    @staticmethod
    def _find_interconnect(dm, index=0):
        devices = dm.ReadAllDevices()
        n = 0
        for d in devices.device_handle:
            if d.device_type == Common_pb2.INTERCONNECT:
                if n == index:
                    return d.device_identifier
                n += 1
        return None

    def configure(self, enable=True):
        cfg = InterconnectConfig_pb2.I2CConfiguration()
        cfg.device     = InterconnectConfig_pb2.I2C_DEVICE_EXPANSION
        cfg.enabled    = enable
        cfg.mode       = InterconnectConfig_pb2.I2C_MODE_STANDARD
        cfg.addressing = InterconnectConfig_pb2.I2C_DEVICE_ADDRESSING_7_BITS
        self._ic.SetI2CConfiguration(cfg, deviceId=self._dev_id)

    def wait_for_ready(self, timeout_s=None):
        """Polls the Arduino status packet until the ready bit (bit0) is set.

        Uses I2C_TIMEOUT_MS for each individual read attempt.
        timeout_s caps the total wait; defaults to BUSY_TIMEOUT_S.
        Returns True if ready, False if timed out.
        """
        if timeout_s is None:
            timeout_s = BUSY_TIMEOUT_S

        poll_interval = I2C_TIMEOUT_MS / 1000.0   # convert ms → seconds
        start_time    = time.time()
        was_busy      = False

        while True:
            is_ready = False   # pessimistic default — comms errors treated as busy

            p = InterconnectConfig_pb2.I2CReadParameter()
            p.device         = InterconnectConfig_pb2.I2C_DEVICE_EXPANSION
            p.device_address = I2C_SLAVE_ADDR
            p.size           = I2C_STATUS_LEN
            p.timeout        = I2C_TIMEOUT_MS

            with self.lock:
                try:
                    resp = self._ic.I2CRead(p, deviceId=self._dev_id)
                    data = resp.data
                    # Validate: correct length, correct start byte, correct checksum
                    if (len(data) == I2C_STATUS_LEN
                            and data[0] == I2C_STATUS_START
                            and xor_checksum(data[:I2C_STATUS_LEN - 1]) == data[I2C_STATUS_LEN - 1]):
                        is_ready = bool(data[1] & 0x01)
                except Exception:
                    pass   # comms glitch → stay in loop

            if is_ready:
                if was_busy:
                    print(" Ready.")
                return True

            elapsed = time.time() - start_time
            if elapsed > timeout_s:
                print(f"\n[ERROR] Gripper still BUSY after {timeout_s:.1f}s — command aborted.")
                return False

            if not was_busy:
                print("Gripper BUSY, waiting...", end="", flush=True)
                was_busy = True

            time.sleep(poll_interval)

    def write_packet(self, cmd: int, args: list = None):
        if args is None:
            args = []
            
        while len(args) < 6:
            args.append(0x00)
            
        payload = [I2C_CMD_START, cmd] + args[:6]
        payload.append(xor_checksum(payload))
        
        p = InterconnectConfig_pb2.I2CWriteParameter()
        p.device         = InterconnectConfig_pb2.I2C_DEVICE_EXPANSION
        p.device_address = I2C_SLAVE_ADDR
        p.data.data      = bytes(payload)
        p.data.size      = I2C_PKT_LEN
        p.timeout        = I2C_TIMEOUT_MS
        
        with self.lock:
            self._ic.I2CWrite(p, deviceId=self._dev_id)

    def read_status(self):
        p = InterconnectConfig_pb2.I2CReadParameter()
        p.device         = InterconnectConfig_pb2.I2C_DEVICE_EXPANSION
        p.device_address = I2C_SLAVE_ADDR
        p.size           = I2C_STATUS_LEN
        p.timeout        = I2C_TIMEOUT_MS
        
        with self.lock:
            resp = self._ic.I2CRead(p, deviceId=self._dev_id)
        
        raw_data = resp.data
        if len(raw_data) != I2C_STATUS_LEN:
            print(f"[WARN] Expected {I2C_STATUS_LEN} bytes, got {len(raw_data)}")
            return
            
        if raw_data[0] != I2C_STATUS_START:
            print(f"[WARN] Bad status start byte: 0x{raw_data[0]:02X}")
            return
            
        calc_xor = xor_checksum(raw_data[:26])
        if calc_xor != raw_data[26]:
            print(f"[WARN] Status checksum mismatch.")
            return
            
        status_byte = raw_data[1]
        is_ready = bool(status_byte & 0x01)
        watchdog_tripped = bool(status_byte & 0x02)
        
        lc1_int, lc2_int, lc3_int = struct.unpack('>iii', raw_data[2:14])
        enc1, enc2, enc3 = struct.unpack('>iii', raw_data[14:26])
        
        print("\n" + "="*55)
        print(f"  SYSTEM STATUS (Ready: {is_ready}, Watchdog: {watchdog_tripped})")
        print("-" * 55)
        print(f"  Motor 1 | Load: {lc1_int / 10.0:6.1f} g | Enc: {enc1:6} (Rel: {enc1 - HOME_POS:+d})")
        print(f"  Motor 2 | Load: {lc2_int / 10.0:6.1f} g | Enc: {enc2:6} (Rel: {enc2 - HOME_POS:+d})")
        print(f"  Motor 3 | Load: {lc3_int / 10.0:6.1f} g | Enc: {enc3:6} (Rel: {enc3 - HOME_POS:+d})")
        print("="*55)


def heartbeat_worker(bridge: I2CBridge, stop_event: threading.Event):
    while not stop_event.is_set():
        try:
            p = InterconnectConfig_pb2.I2CWriteParameter()
            p.device         = InterconnectConfig_pb2.I2C_DEVICE_EXPANSION
            p.device_address = I2C_SLAVE_ADDR
            p.data.data      = bytes([CMD_HEARTBEAT])
            p.data.size      = 1
            p.timeout        = I2C_TIMEOUT_MS
            with bridge.lock:
                bridge._ic.I2CWrite(p, deviceId=bridge._dev_id)
        except Exception:
            pass
        time.sleep(1.0)


def main():
    args = utilities.parseConnectionArguments()
    
    with utilities.DeviceConnection.createTcpConnection(args) as router:
        bridge = I2CBridge(router)
        bridge.configure(enable=True)
        time.sleep(0.3)
        
        stop_event = threading.Event()
        hb_thread = threading.Thread(target=heartbeat_worker, args=(bridge, stop_event), daemon=True)
        hb_thread.start()

        print("\n=== Gripper I2C Control CLI ===")
        print("Commands: set <id> <pos> | setall <m1> <m2> <m3> | stop | tare | status | quit\n")

        try:
            while True:
                cmd_in = input("gripper> ").strip().lower().split()
                if not cmd_in:
                    continue
                    
                action = cmd_in[0]
                
                try:
                    if action in ["quit", "exit"]:
                        break
                        
                    elif action == "stop":
                        bridge.write_packet(CMD_STOP_ALL)
                        print("Sent: STOP_ALL")
                        
                    elif action == "set":
                        if len(cmd_in) != 3:
                            print("Usage: set <motor_id> <pos>")
                            continue
                        
                        if not bridge.wait_for_ready():
                            continue
                            
                        motor_id = int(cmd_in[1])
                        offset_pos = int(cmd_in[2])
                        abs_pos = HOME_POS + offset_pos 
                        
                        val = abs_pos & 0xFFFF 
                        bridge.write_packet(CMD_SET_POS, [motor_id, (val >> 8) & 0xFF, val & 0xFF])
                        print(f"Sent: SET_POS M{motor_id} -> {abs_pos}")
                        
                    elif action == "setall":
                        if len(cmd_in) != 4:
                            print("Usage: setall <m1> <m2> <m3> (use 'x' to skip)")
                            continue
                        
                        if not bridge.wait_for_ready():
                            continue
                        
                        motor_args = []
                        for i in range(1, 4):
                            val_str = cmd_in[i]
                            if val_str == 'x':
                                motor_args.extend([0xFF, 0xFF])
                            else:
                                abs_pos = HOME_POS + int(val_str)
                                val = abs_pos & 0xFFFF
                                motor_args.extend([(val >> 8) & 0xFF, val & 0xFF])

                        bridge.write_packet(CMD_SET_POS_ALL, motor_args)
                        print(f"Sent: SET_ALL")
                        
                    elif action == "tare":
                        if len(cmd_in) == 2:
                            motor_id = int(cmd_in[1])
                            bridge.write_packet(CMD_TARE_ONE, [motor_id])
                            print(f"Sent: TARE_ONE M{motor_id}")
                        else:
                            bridge.write_packet(CMD_TARE)
                            print("Sent: TARE ALL")
                            
                    elif action == "status":
                        bridge.read_status()
                        
                    else:
                        print("Unknown command.")
                        
                except ValueError:
                    print("Error: Invalid number format.")
                except Exception as e:
                    print(f"I2C Communication Error: {e}")
                    
        finally:
            stop_event.set()
            bridge.write_packet(CMD_STOP_ALL)
            bridge.configure(enable=False)
            print("Exiting and cleaning up.")

if __name__ == "__main__":
    main()