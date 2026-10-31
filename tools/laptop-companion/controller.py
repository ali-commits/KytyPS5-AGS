"""SDL2 owns Bluetooth framing, CRC and sensor calibration; no raw HID access."""
import ctypes as C
import ctypes.util
import math
import time
# SDL GameController buttons mapped to Moonlight/Limelight flags.
BUTTONS = {
    0: 4096,
    1: 8192,
    2: 16384,
    3: 32768,
    4: 32,
    5: 1024,
    6: 16,
    7: 64,
    8: 128,
    9: 256,
    10: 512,
    11: 1,
    12: 2,
    13: 4,
    14: 8,
    20: 1048576,
}

def buttons(pressed):
    return sum((mask for button, mask in BUTTONS.items() if button in pressed))

def axes(raw):
    return [max(-1, min(1, v / (32768 if v < 0 else 32767))) if i < 4 else max(0, min(1, v / 32767)) for i, v in enumerate(raw)]

def trigger_report(flags, left, right):
    if type(flags) is not int or flags & ~0x0C:
        raise ValueError('Invalid trigger flags')
    for effect in (left, right):
        if len(effect) != 11 or any((type(v) is not int or not 0 <= v <= 255 for v in effect)):
            raise ValueError('Invalid trigger effect')
    # SDL DS5EffectsState_t, not USB/Bluetooth framing.
    # Enable bit 2 is right; bit 3 is left. Right data starts at 10, left at 21.
    p = bytearray(47)
    p[0] = flags
    p[10:21] = bytes(right)
    p[21:32] = bytes(left)
    return bytes(p)
OFF = [5] + [0] * 10

class Feedback:

    def __init__(self, controller):
        self.controller = controller
        self.deadline = 0

    def apply(self, data, now):
        lease = data['lease_ms']
        rumble = data['rumble']
        t = data['triggers']
        if type(lease) is not int or not 0 < lease <= 200:
            raise ValueError('Invalid feedback lease')
        if len(rumble) != 2 or any((type(v) is not int or not 0 <= v <= 65535 for v in rumble)):
            raise ValueError('Invalid rumble')
        report = trigger_report(t['flags'], t['left'], t['right'])
        self.controller.effects(rumble, report, lease, deadline=now + lease / 1000)
        self.deadline = now + lease / 1000

    def expire(self, now):
        if self.deadline and now >= self.deadline:
            self.stop()

    def stop(self):
        self.controller.stop()
        self.deadline = 0

class Controller:

    def __init__(self):
        self.s = C.CDLL(ctypes.util.find_library('SDL2') or 'libSDL2-2.0.so.0')
        self.pad = None
        self.output = None
        self.serial = None
        signatures = {
            'SDL_SetHint': (C.c_int, [C.c_char_p, C.c_char_p]),
            'SDL_Init': (C.c_int, [C.c_uint32]),
            'SDL_GetError': (C.c_char_p, []),
            'SDL_NumJoysticks': (C.c_int, []),
            'SDL_IsGameController': (C.c_int, [C.c_int]),
            'SDL_GameControllerOpen': (C.c_void_p, [C.c_int]),
            'SDL_GameControllerGetAttached': (C.c_int, [C.c_void_p]),
            'SDL_GameControllerClose': (None, [C.c_void_p]),
            'SDL_GameControllerGetButton': (C.c_uint8, [C.c_void_p, C.c_int]),
            'SDL_GameControllerGetAxis': (C.c_int16, [C.c_void_p, C.c_int]),
            'SDL_GameControllerGetType': (C.c_int, [C.c_void_p]),
            'SDL_GameControllerGetSerial': (C.c_char_p, [C.c_void_p]),
            'SDL_GameControllerGetJoystick': (C.c_void_p, [C.c_void_p]),
            'SDL_JoystickCurrentPowerLevel': (C.c_int, [C.c_void_p]),
            'SDL_GameControllerSetSensorEnabled': (C.c_int, [C.c_void_p, C.c_int, C.c_int]),
            'SDL_GameControllerGetSensorData': (C.c_int, [C.c_void_p, C.c_int, C.POINTER(C.c_float), C.c_int]),
            'SDL_GameControllerRumble': (C.c_int, [C.c_void_p, C.c_uint16, C.c_uint16, C.c_uint32]),
            'SDL_GameControllerSendEffect': (C.c_int, [C.c_void_p, C.c_void_p, C.c_int]),
            'SDL_PumpEvents': (None, []),
            'SDL_GameControllerUpdate': (None, []),
            'SDL_Quit': (None, []),
        }
        for name, (result, args) in signatures.items():
            f = getattr(self.s, name)
            f.restype = result
            f.argtypes = args
        for name in ('SDL_JOYSTICK_HIDAPI', 'SDL_JOYSTICK_HIDAPI_PS5', 'SDL_JOYSTICK_HIDAPI_PS5_RUMBLE', 'SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS'):
            self.s.SDL_SetHint(name.encode(), b'1')
        if self.s.SDL_Init(0x2000 | 0x200 | 0x4000):
            raise RuntimeError(self.error())

    def error(self):
        return self.s.SDL_GetError().decode(errors='replace')

    def snapshot(self):
        self.s.SDL_PumpEvents()
        self.s.SDL_GameControllerUpdate()
        if self.pad and (not self.s.SDL_GameControllerGetAttached(self.pad)):
            self.s.SDL_GameControllerClose(self.pad)
            self.pad = None
        if not self.pad:
            for i in range(self.s.SDL_NumJoysticks()):
                if not self.s.SDL_IsGameController(i):
                    continue
                p = self.s.SDL_GameControllerOpen(i)
                if not p:
                    continue
                if self.s.SDL_GameControllerGetType(p) != 7:  # SDL_CONTROLLER_TYPE_PS5
                    self.s.SDL_GameControllerClose(p)
                    continue
                self.pad = p
                serial = self.s.SDL_GameControllerGetSerial(p)
                self.serial = serial.decode(errors='replace') if serial else None
                for sensor in (1, 2):
                    self.s.SDL_GameControllerSetSensorEnabled(p, sensor, 1)
                break
        result = {'connected': bool(self.pad), 'buttons': 0, 'axes': [0.0] * 6, 'accel': [0.0] * 3, 'gyro': [0.0] * 3}
        self.battery = 'No DualSense'
        if self.pad:
            result['buttons'] = buttons({i for i in BUTTONS if self.s.SDL_GameControllerGetButton(self.pad, i)})
            result['axes'] = axes([self.s.SDL_GameControllerGetAxis(self.pad, i) for i in range(6)])
            for name, sensor, limit in (('accel', 1, 160), ('gyro', 2, 40)):
                values = (C.c_float * 3)()
                if self.s.SDL_GameControllerGetSensorData(self.pad, sensor, values, 3) == 0:
                    result[name] = [max(-limit, min(limit, float(v))) if math.isfinite(v) else 0 for v in values]
            power = self.s.SDL_JoystickCurrentPowerLevel(self.s.SDL_GameControllerGetJoystick(self.pad))
            self.battery = {-1: 'Unknown', 0: 'Empty', 1: 'Low', 2: 'Medium', 3: 'Full', 4: 'Wired', 5: 'Unknown'}.get(power, 'Unknown')
        return result

    def attach_output(self, output):
        """Cancel SDL's old rumble before a single raw writer takes ownership."""
        if getattr(self, 'output', None) is not None:
            raise RuntimeError('A controller output writer is already attached')
        self.stop()
        if output.acquire() is False:
            output.shutdown()
            raise RuntimeError(getattr(output, 'status', 'Controller output unavailable'))
        self.output = output

    def detach_output(self):
        output = getattr(self, 'output', None)
        if output is not None:
            output.shutdown()
            self.output = None
            self.stop()

    def effects(self, rumble, report, lease, *, deadline=None):
        output = getattr(self, 'output', None)
        if output is not None and output.owns_output:
            feedback = {'lease_ms': lease, 'rumble': rumble,
                        'triggers': {'flags': report[0], 'left': list(report[21:32]),
                                     'right': list(report[10:21])}}
            output.controls(feedback, deadline if deadline is not None else time.monotonic() + lease / 1000)
            return
        if not self.pad:
            return
        # Trigger update first; SDL rumble maintains its own state.
        buf = C.create_string_buffer(report)
        if self.s.SDL_GameControllerSendEffect(self.pad, buf, len(report)) < 0:
            raise RuntimeError(self.error())
        if self.s.SDL_GameControllerRumble(self.pad, *rumble, lease) < 0:
            raise RuntimeError(self.error())

    def stop(self):
        output = getattr(self, 'output', None)
        if output is not None and output.owns_output:
            output.controls({'lease_ms': 1, 'rumble': [0, 0],
                             'triggers': {'flags': 12, 'left': OFF, 'right': OFF}}, 0)
            return
        if self.pad:
            self.s.SDL_GameControllerRumble(self.pad, 0, 0, 0)
            p = trigger_report(12, OFF, OFF)
            self.s.SDL_GameControllerSendEffect(self.pad, C.create_string_buffer(p), len(p))

    def close(self):
        self.detach_output()
        self.stop()
        if self.pad:
            self.s.SDL_GameControllerClose(self.pad)
        self.pad = None
        self.s.SDL_Quit()
