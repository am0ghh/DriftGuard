import asyncio
import time
from bleak import BleakClient, BleakScanner
from pynput.mouse import Button, Controller

DEVICE_NAME = "DriftGuard"
CHARACTERISTIC_UUID = "abcd1234-ab12-ab12-ab12-abcdef123456"
COMMAND_UUID        = "abcd1234-ab12-ab12-ab12-abcdef123457"

SMOOTH = 0.2
EXPONENT = 2.0
SCROLL_HOLD       = 0.3  # hold + joystick movement → scroll mode
RIGHT_CLICK_HOLD  = 0.6  # hold + joystick still   → right click on release
DWELL_TIME = 1.5         # seconds still before dwell click fires
SCROLL_SPEED = 0.1       # scroll units per joystick unit per frame

mouse = Controller()
command_queue = asyncio.Queue()

prev_x = 0.0
prev_y = 0.0
remainder_x = 0.0
remainder_y = 0.0
scroll_remainder_x = 0.0
scroll_remainder_y = 0.0

btn_prev = 1
btn_press_time = None
scroll_active = False
scroll_active_prev = False
right_click_armed = False

dwell_start_time = 0.0
dwell_fired = False
dwell_counting = False


def response_curve(value):
    sign = 1 if value >= 0 else -1
    normalized = abs(value) / 100.0
    curved = normalized ** EXPONENT
    return sign * curved * 100.0


def handle_data(sender, data):
    global prev_x, prev_y, remainder_x, remainder_y
    global scroll_remainder_x, scroll_remainder_y
    global btn_prev, btn_press_time, scroll_active, scroll_active_prev, right_click_armed
    global dwell_start_time, dwell_fired, dwell_counting

    try:
        decoded = data.decode("utf-8")
        parts = decoded.split(",")

        if len(parts) != 4:
            return

        x = int(parts[0])
        y = int(parts[1])
        btn = int(parts[2])
        speed = int(parts[3])

        current_time = time.time()

        # initialise dwell timer on first frame
        if dwell_start_time == 0.0:
            dwell_start_time = current_time

        cx = response_curve(x)
        cy = response_curve(y)

        sx = SMOOTH * (cy) + (1 - SMOOTH) * prev_x
        sy = SMOOTH * (-cx) + (1 - SMOOTH) * prev_y
        prev_x = sx
        prev_y = sy

        # --- Button state machine ---
        btn_just_pressed  = (btn == 0 and btn_prev == 1)
        btn_just_released = (btn == 1 and btn_prev == 0)
        joystick_moving   = (x != 0 or y != 0)

        if btn_just_pressed:
            btn_press_time = current_time

        if btn == 0 and btn_press_time is not None and not scroll_active and not right_click_armed:
            held_duration = current_time - btn_press_time
            if joystick_moving and held_duration > SCROLL_HOLD:
                scroll_active = True
            elif not joystick_moving and held_duration > RIGHT_CLICK_HOLD:
                right_click_armed = True

        if btn_just_released:
            if scroll_active:
                scroll_active = False
            elif right_click_armed:
                mouse.click(Button.right)
                print("Right click!")
                command_queue.put_nowait(b'1')
                right_click_armed = False
            else:
                mouse.click(Button.left)
                print("Click!")
                command_queue.put_nowait(b'1')

        btn_prev = btn

        # --- Scroll mode LED ---
        if scroll_active and not scroll_active_prev:
            command_queue.put_nowait(b'3')
        elif not scroll_active and scroll_active_prev:
            command_queue.put_nowait(b'4')
        scroll_active_prev = scroll_active

        # --- Movement / Scroll ---
        cursor_moved = False

        if scroll_active:
            scroll_remainder_x += sx * SCROLL_SPEED
            scroll_remainder_y += sy * SCROLL_SPEED
            isx = int(scroll_remainder_x)
            isy = int(scroll_remainder_y)
            scroll_remainder_x -= isx
            scroll_remainder_y -= isy
            if isx != 0 or isy != 0:
                mouse.scroll(isx, -isy)
        else:
            remainder_x += sx * speed / 100
            remainder_y += sy * speed / 100
            ix = int(remainder_x)
            iy = int(remainder_y)
            remainder_x -= ix
            remainder_y -= iy
            if ix != 0 or iy != 0:
                mouse.move(ix, iy)
                cursor_moved = True

        # --- Dwell click ---
        if cursor_moved:
            dwell_start_time = current_time
            dwell_fired = False
            if dwell_counting:
                dwell_counting = False
                command_queue.put_nowait(b'6')  # dwell reset → back to connected
        elif not dwell_fired and not scroll_active:
            if not dwell_counting:
                dwell_counting = True
                command_queue.put_nowait(b'5')  # dwell countdown start
            if current_time - dwell_start_time > DWELL_TIME:
                mouse.click(Button.left)
                print("Dwell click!")
                command_queue.put_nowait(b'1')
                dwell_counting = False
                command_queue.put_nowait(b'6')  # back to connected after click
                dwell_fired = True

    except Exception as e:
        print(f"Error parsing data: {e}")


async def main():
    print("Scanning for DriftGuard...")

    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10)

    if device is None:
        print("DriftGuard not found. Make sure ESP32 is powered and advertising.")
        return

    print(f"Found DriftGuard at {device.address}")
    print("Connecting...")

    async with BleakClient(device) as client:
        print("Connected! Move joystick to control mouse.")
        print("Controls:")
        print("  Tap button          → left click")
        print("  Double tap button   → right click")
        print("  Hold button + move  → scroll")
        print("  Stay still 1.5s     → dwell click")
        print("Press Ctrl+C to stop.")

        await client.start_notify(CHARACTERISTIC_UUID, handle_data)

        while client.is_connected:
            await asyncio.sleep(0.01)
            while not command_queue.empty():
                cmd = command_queue.get_nowait()
                try:
                    await client.write_gatt_char(COMMAND_UUID, cmd)
                except Exception:
                    pass

    print("Disconnected.")


if __name__ == "__main__":
    asyncio.run(main())
