#!/usr/bin/env python3

###
# Control -- W,S  E,D  R,F  T,G
# New Control -- L (Load Cell Read), Z (Load Cell Zero/Tare)
###

import sys
import os
import time
import signal
from pynput import keyboard
from kortex_api.autogen.client_stubs.DeviceManagerClientRpc import DeviceManagerClient
from kortex_api.autogen.client_stubs.InterconnectConfigClientRpc import InterconnectConfigClient
from kortex_api.autogen.messages import Common_pb2
from kortex_api.autogen.messages import InterconnectConfig_pb2

last_action_time = time.time()
# Increased cooldown to allow motor time to move before accepting a new command
cooldown_duration = 0.75  # seconds

# Set to track which motor keys are currently down.
# This prevents sending multiple commands while a key is held.
currently_pressed_motor_keys = set()

MOTOR_KEYS = {
    keyboard.KeyCode.from_char('w'), keyboard.KeyCode.from_char('s'),
    keyboard.KeyCode.from_char('e'), keyboard.KeyCode.from_char('d'),
    keyboard.KeyCode.from_char('r'), keyboard.KeyCode.from_char('f'),
    keyboard.KeyCode.from_char('t'), keyboard.KeyCode.from_char('g')
}


class I2CBridge:
    def __init__(self, router):
        self.router = router
        self.device_manager = DeviceManagerClient(self.router)
        self.interconnect_config = InterconnectConfigClient(self.router)
        self.interconnect_device_id = self.GetDeviceIdFromDevType(Common_pb2.INTERCONNECT)
        if self.interconnect_device_id is None:
            print("Could not find Interconnect, exiting...")
            sys.exit(1)

    def GetDeviceIdFromDevType(self, device_type, device_index=0):
        devices = self.device_manager.ReadAllDevices()
        current_index = 0
        for device in devices.device_handle:
            if device.device_type == device_type:
                if current_index == device_index:
                    return device.device_identifier
                current_index += 1
        return None

    def WriteValue(self, device_address, data, timeout_ms):
        i2c_write = InterconnectConfig_pb2.I2CWriteParameter()
        i2c_write.device = InterconnectConfig_pb2.I2C_DEVICE_EXPANSION
        i2c_write.device_address = device_address
        i2c_write.data.data = bytes(data)
        i2c_write.data.size = len(data)
        i2c_write.timeout = timeout_ms
        self.interconnect_config.I2CWrite(i2c_write, deviceId=self.interconnect_device_id)

    def ReadValue(self, device_address, read_size, timeout_ms):
        i2c_read = InterconnectConfig_pb2.I2CReadParameter()
        i2c_read.device = InterconnectConfig_pb2.I2C_DEVICE_EXPANSION
        i2c_read.device_address = device_address
        i2c_read.size = read_size
        i2c_read.timeout = timeout_ms

        response = self.interconnect_config.I2CRead(i2c_read, deviceId=self.interconnect_device_id)

        return response.data.decode('utf-8')

    def Configure(self, enable=True):
        config = InterconnectConfig_pb2.I2CConfiguration()
        config.device = InterconnectConfig_pb2.I2C_DEVICE_EXPANSION
        config.enabled = enable
        config.mode = InterconnectConfig_pb2.I2C_MODE_STANDARD
        config.addressing = InterconnectConfig_pb2.I2C_DEVICE_ADDRESSING_7_BITS
        self.interconnect_config.SetI2CConfiguration(config, deviceId=self.interconnect_device_id)


def stop_motors_i2c():
    """Sends the 0x09 command to the ESP32 to stop the motors."""
    print("Stopping Motors (0x09)")
    message = "0x09"
    buf = message.encode('utf-8')
    try:
        bridge.WriteValue(slave_address, buf, 100)
    except Exception as e:
        print(f"Error sending stop command: {e}")


def on_press(key):
    global last_action_time
    global cooldown_duration

    char_key = None
    if hasattr(key, 'char'):
        char_key = keyboard.KeyCode.from_char(key.char)
    elif key in MOTOR_KEYS:
        char_key = key

    try:
        # Handle ESC key immediately
        if key == keyboard.Key.esc:
            print("Ending Program and Stopping Motors")
            stop_motors_i2c()
            listener.stop()
            bridge.Configure(enable=False)
            sys.exit(0)

        # 1. Enforce single command per key press for motor keys
        is_motor_key = char_key in MOTOR_KEYS
        if is_motor_key:
            if char_key in currently_pressed_motor_keys:
                # Key is already held down and command was sent, so ignore
                return
            # Key is newly pressed, add it to the set
            currently_pressed_motor_keys.add(char_key)

        # 2. Enforce cooldown for all command types
        if time.time() - last_action_time < cooldown_duration:
            if is_motor_key:
                # If we are in cooldown for a new motor command, remove the key from the set
                currently_pressed_motor_keys.discard(char_key)
            return

        # --- Command Execution (Passed all checks) ---
        last_action_time = time.time()

        # Motor Control Keys (W, S, E, D, R, F, T, G)
        if is_motor_key:

            command = None
            if char_key == keyboard.KeyCode.from_char('w'):
                command = "0x01"; print("W pressed: Incrementing Motor 1 (0x01)")
            elif char_key == keyboard.KeyCode.from_char('s'):
                command = "0x02"; print("S pressed: Decrementing Motor 1 (0x02)")
            elif char_key == keyboard.KeyCode.from_char('e'):
                command = "0x03"; print("E pressed: Incrementing Motor 2 (0x03)")
            elif char_key == keyboard.KeyCode.from_char('d'):
                command = "0x04"; print("D pressed: Decrementing Motor 2 (0x04)")
            elif char_key == keyboard.KeyCode.from_char('r'):
                command = "0x05"; print("R pressed: Incrementing Motor 3 (0x05)")
            elif char_key == keyboard.KeyCode.from_char('f'):
                command = "0x06"; print("F pressed: Decrementing Motor 3 (0x06)")
            elif char_key == keyboard.KeyCode.from_char('t'):
                command = "0x07"; print("T pressed: Incrementing All Motors (0x07)")
            elif char_key == keyboard.KeyCode.from_char('g'):
                command = "0x08"; print("G pressed: Decrementing All Motors (0x08)")

            if command:
                buf = command.encode('utf-8')
                bridge.WriteValue(slave_address, buf, 100)

        # Utility Keys (L, Z, O, P) - These still respect the cooldown
        elif char_key == keyboard.KeyCode.from_char('o'):
            print("O pressed: Resetting all encoders to 0 (0x10)")
            message = "0x10"
            buf = message.encode('utf-8')
            bridge.WriteValue(slave_address, buf, 100)

        elif char_key == keyboard.KeyCode.from_char('p'):
            print("P pressed: Setting all encoders to max value (0x11)")
            message = "0x11"
            buf = message.encode('utf-8')
            bridge.WriteValue(slave_address, buf, 100)

        elif char_key == keyboard.KeyCode.from_char('l'):
            read_size = 40
            timeout = 100
            try:
                data_str = bridge.ReadValue(slave_address, read_size, timeout)
                print("-" * 40)
                print(f"LOAD CELL READINGS (I2C): {data_str.strip()}")
                print("-" * 40)
            except Exception as e:
                print(f"Error reading load cells via I2C: {e}")

        elif char_key == keyboard.KeyCode.from_char('z'):
            print("Z pressed: Zeroing Load Cells (0x13)")
            message = "0x13"
            buf = message.encode('utf-8')
            bridge.WriteValue(slave_address, buf, 100)

    except Exception as e:
        print(f"Error in on_press for key {key}: {e}")


def on_release(key):
    # Only remove the key from the tracking set when released.
    # NO stop command is sent to avoid race conditions.
    if hasattr(key, 'char'):
        char_key = keyboard.KeyCode.from_char(key.char)
    else:
        char_key = key

    if char_key in currently_pressed_motor_keys:
        currently_pressed_motor_keys.discard(char_key)


def signal_handler(sig, frame):
    print("\nCtrl+C received, stopping...")
    stop_motors_i2c()
    listener.stop()
    bridge.Configure(enable=False)
    sys.exit(0)


def main():
    global bridge, slave_address, listener
    import argparse
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
    import utilities

    parser = argparse.ArgumentParser()
    args = utilities.parseConnectionArguments(parser)

    slave_address = 0x08
    with utilities.DeviceConnection.createTcpConnection(args) as router:
        bridge = I2CBridge(router)
        bridge.Configure()
        time.sleep(1)
        print(
            "I2C bridge initialized. Keys: (W,S: Motor1; E,D: Motor2; R,F: Motor3, L: Read Load Cells, Z: Zero Load Cells), ESC to exit.")
        signal.signal(signal.SIGINT, signal_handler)

        # Start keyboard listener for press AND release events
        listener = keyboard.Listener(on_press=on_press, on_release=on_release)
        listener.start()
        try:
            listener.join()
        except KeyboardInterrupt:
            signal_handler(None, None)


if __name__ == "__main__":
    main()
