#include <cmath>
#include <endian.h>
#include <inputtino/input.hpp>
#include <uhid/protected_types.hpp>
#include <uhid/ps5.hpp>
#include <uhid/uhid.hpp>
#include <random>

namespace inputtino {

static void send_report(PS5JoypadState &state) {
  { // setup timestamp and increase seq_number
    state.current_state.seq_number++;
    if (state.current_state.seq_number >= 255) {
      state.current_state.seq_number = 0;
    }

    // Seems that the timestamp is little endian and 0.33us units
    // see:
    // https://github.com/torvalds/linux/blob/305230142ae0637213bf6e04f6d9f10bbcb74af8/drivers/hid/hid-playstation.c#L1409-L1410
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                   .count();
    state.current_state.sensor_timestamp = htole32(now / 333);
  }

  struct uhid_event ev {};
  {
    ev.type = UHID_INPUT2;
    unsigned char *data = (unsigned char *)&state.current_state;
    std::copy(data, data + sizeof(state.current_state), &ev.u.input2.data[0]);
    ev.u.input2.size = sizeof(state.current_state);
  }
  state.dev->send(ev);
}

static void on_uhid_event(std::shared_ptr<PS5JoypadState> state, uhid_event ev, int fd) {
  switch (ev.type) {
  case UHID_GET_REPORT: {
    uhid_event answer{};
    answer.type = UHID_GET_REPORT_REPLY;
    answer.u.get_report_reply.id = ev.u.get_report.id;
    answer.u.get_report_reply.err = 0;
    switch (ev.u.get_report.rnum) {
    case uhid::PS5_REPORT_TYPES::CALIBRATION: {
      std::copy(&uhid::ps5_calibration_info[0],
                &uhid::ps5_calibration_info[0] + sizeof(uhid::ps5_calibration_info),
                &answer.u.get_report_reply.data[0]);
      answer.u.get_report_reply.size = sizeof(uhid::ps5_calibration_info);
      break;
    }
    case uhid::PS5_REPORT_TYPES::PAIRING_INFO: {
      std::copy(&uhid::ps5_pairing_info[0],
                &uhid::ps5_pairing_info[0] + sizeof(uhid::ps5_pairing_info),
                &answer.u.get_report_reply.data[0]);

      // Copy MAC address data
      std::reverse_copy(&state->mac_address[0],
                        &state->mac_address[0] + sizeof(state->mac_address),
                        &answer.u.get_report_reply.data[1]);

      answer.u.get_report_reply.size = sizeof(uhid::ps5_pairing_info);
      break;
    }
    case uhid::PS5_REPORT_TYPES::FIRMWARE_INFO: {
      std::copy(&uhid::ps5_firmware_info[0],
                &uhid::ps5_firmware_info[0] + sizeof(uhid::ps5_firmware_info),
                &answer.u.get_report_reply.data[0]);
      answer.u.get_report_reply.size = sizeof(uhid::ps5_firmware_info);
      break;
    }
    default:
      answer.u.get_report_reply.err = -EINVAL;
      break;
    }
    auto res = uhid::uhid_write(fd, &answer);
    // TODO: signal error somehow
    break;
  }
  case UHID_OUTPUT: { // This is sent if the HID device driver wants to send raw data to the device
    // Here is where we'll get Rumble and LED events
    uhid::dualsense_output_report_usb *report = (uhid::dualsense_output_report_usb *)ev.u.output.data;
    /*
     * RUMBLE
     * The PS5 joypad seems to report values in the range 0-255,
     * we'll turn those into 0-0xFFFF
     */
    if (report->valid_flag0 & uhid::MOTOR_OR_COMPATIBLE_VIBRATION || report->valid_flag2 & uhid::COMPATIBLE_VIBRATION) {
      auto left = (report->motor_left / 255.0f) * 0xFFFF;
      auto right = (report->motor_right / 255.0f) * 0xFFFF;
      if (state->on_rumble) {
        (*state->on_rumble)(left, right);
      }
    }

    /*
     * LED
     */
    if (report->valid_flag1 & uhid::LIGHTBAR_ENABLE) {
      if (state->on_led) {
        // TODO: should we blend brightness?
        (*state->on_led)(report->lightbar_red, report->lightbar_green, report->lightbar_blue);
      }
    }
  }
  default:
    break;
  }
}

void generate_mac_address(PS5JoypadState *state) {
  std::default_random_engine generator;
  std::uniform_int_distribution<unsigned char> distribution(0, 0xFF);
  for (int i = 0; i < 6; i++) {
    state->mac_address[i] = distribution(generator);
  }
}

PS5Joypad::PS5Joypad() : _state(std::make_shared<PS5JoypadState>()) {
  generate_mac_address(this->_state.get());
}

PS5Joypad::~PS5Joypad() {
  if (this->_state && this->_state->dev) {
    this->_state->dev->stop_thread();
    this->_state->dev.reset(); // Will trigger ~Device and ultimately destroy the device
  }
}

Result<PS5Joypad> PS5Joypad::create(const DeviceDefinition &device) {
  auto def = uhid::DeviceDefinition{
      .name = device.name,
      .phys = device.device_phys,
      .uniq = device.device_uniq,
      .bus = BUS_USB,
      .vendor = static_cast<uint32_t>(device.vendor_id),
      .product = static_cast<uint32_t>(device.product_id),
      .version = static_cast<uint32_t>(device.version),
      .country = 0,
      .report_description = {&uhid::ps5_rdesc[0], &uhid::ps5_rdesc[0] + sizeof(uhid::ps5_rdesc)}};

  auto joypad = PS5Joypad();
  auto dev =
      uhid::Device::create(def, [state = joypad._state](uhid_event ev, int fd) { on_uhid_event(state, ev, fd); });
  if (dev) {
    joypad._state->dev = std::make_shared<uhid::Device>(std::move(*dev));
    return joypad;
  }
  return Error(dev.getErrorMessage());
}

static int scale_value(int input, int input_start, int input_end, int output_start, int output_end) {
  auto slope = 1.0 * (output_end - output_start) / (input_end - input_start);
  return output_start + std::round(slope * (input - input_start));
}

std::vector<std::string> PS5Joypad::get_nodes() const {
  // TODO
  return std::vector<std::string>();
}

void PS5Joypad::set_pressed_buttons(int pressed) {
  { // First reset everything to non-pressed
    this->_state->current_state.buttons[0] = 0;
    this->_state->current_state.buttons[1] = 0;
    this->_state->current_state.buttons[2] = 0;
    this->_state->current_state.buttons[3] = 0;
  }
  {
    if (DPAD_UP & pressed) {     // Pressed UP
      if (DPAD_LEFT & pressed) { // NW
        this->_state->current_state.buttons[0] |= uhid::HAT_NW;
      } else if (DPAD_RIGHT & pressed) { // NE
        this->_state->current_state.buttons[0] |= uhid::HAT_NE;
      } else { // N
        this->_state->current_state.buttons[0] |= uhid::HAT_N;
      }
    }

    if (DPAD_DOWN & pressed) {   // Pressed DOWN
      if (DPAD_LEFT & pressed) { // SW
        this->_state->current_state.buttons[0] |= uhid::HAT_SW;
      } else if (DPAD_RIGHT & pressed) { // SE
        this->_state->current_state.buttons[0] |= uhid::HAT_SE;
      } else { // S
        this->_state->current_state.buttons[0] |= uhid::HAT_S;
      }
    }

    if (DPAD_LEFT & pressed) {                              // Pressed LEFT
      if (!(DPAD_UP & pressed) && !(DPAD_DOWN & pressed)) { // Pressed only LEFT
        this->_state->current_state.buttons[0] |= uhid::HAT_W;
      }
    }

    if (DPAD_RIGHT & pressed) {                             // Pressed RIGHT
      if (!(DPAD_UP & pressed) && !(DPAD_DOWN & pressed)) { // Pressed only RIGHT
        this->_state->current_state.buttons[0] |= uhid::HAT_E;
      }
    }

    if (!(DPAD_UP & pressed) && !(DPAD_DOWN & pressed) && !(DPAD_LEFT & pressed) && !(DPAD_RIGHT & pressed)) {
      this->_state->current_state.buttons[0] |= uhid::HAT_NEUTRAL;
    }

    // TODO: L2/R2 ??

    if (X & pressed)
      this->_state->current_state.buttons[0] |= uhid::SQUARE;
    if (Y & pressed)
      this->_state->current_state.buttons[0] |= uhid::TRIANGLE;
    if (A & pressed)
      this->_state->current_state.buttons[0] |= uhid::CROSS;
    if (B & pressed)
      this->_state->current_state.buttons[0] |= uhid::CIRCLE;
    if (LEFT_BUTTON & pressed)
      this->_state->current_state.buttons[1] |= uhid::L1;
    if (RIGHT_BUTTON & pressed)
      this->_state->current_state.buttons[1] |= uhid::R1;
    if (LEFT_STICK & pressed)
      this->_state->current_state.buttons[1] |= uhid::L3;
    if (RIGHT_STICK & pressed)
      this->_state->current_state.buttons[1] |= uhid::R3;
    if (START & pressed)
      this->_state->current_state.buttons[1] |= uhid::OPTIONS;
    if (BACK & pressed)
      this->_state->current_state.buttons[1] |= uhid::CREATE;
    if (TOUCHPAD_FLAG & pressed)
      this->_state->current_state.buttons[2] |= uhid::TOUCHPAD;
    if (HOME & pressed)
      this->_state->current_state.buttons[2] |= uhid::PS_HOME;
    if (MISC_FLAG & pressed)
      this->_state->current_state.buttons[2] |= uhid::MIC_MUTE;
  }
  send_report(*this->_state);
}
void PS5Joypad::set_triggers(int16_t left, int16_t right) {
  this->_state->current_state.z = scale_value(left, 0, 255, uhid::PS5_AXIS_MIN, uhid::PS5_AXIS_MAX);
  this->_state->current_state.rz = scale_value(right, 0, 255, uhid::PS5_AXIS_MIN, uhid::PS5_AXIS_MAX);
  send_report(*this->_state);
}
void PS5Joypad::set_stick(Joypad::STICK_POSITION stick_type, short x, short y) {
  switch (stick_type) {
  case RS: {
    this->_state->current_state.rx = scale_value(x, -32768, 32767, uhid::PS5_AXIS_MIN, uhid::PS5_AXIS_MAX);
    this->_state->current_state.ry = scale_value(y, -32768, 32767, uhid::PS5_AXIS_MIN, uhid::PS5_AXIS_MAX);
    send_report(*this->_state);
    break;
  }
  case LS: {
    this->_state->current_state.x = scale_value(x, -32768, 32767, uhid::PS5_AXIS_MIN, uhid::PS5_AXIS_MAX);
    this->_state->current_state.y = scale_value(y, -32768, 32767, uhid::PS5_AXIS_MIN, uhid::PS5_AXIS_MAX);
    send_report(*this->_state);
    break;
  }
  }
}
void PS5Joypad::set_on_rumble(const std::function<void(int, int)> &callback) {
  this->_state->on_rumble = callback;
}

static inline float rad2deg(float rad) {
  return rad * (180.0f / (float)M_PI);
}

void PS5Joypad::set_motion(PS5Joypad::MOTION_TYPE type, float x, float y, float z) {
  switch (type) {
  case ACCELERATION: {
    this->_state->current_state.accel[0] = htole16((x * uhid::SDL_STANDARD_GRAVITY * 100));
    this->_state->current_state.accel[1] = htole16((y * uhid::SDL_STANDARD_GRAVITY * 100));
    this->_state->current_state.accel[2] = htole16((z * uhid::SDL_STANDARD_GRAVITY * 100));
    send_report(*this->_state);
    break;
  }
  case GYROSCOPE: {
    this->_state->current_state.gyro[0] =
        htole16(rad2deg((x + uhid::gyro_calib_bias) / uhid::gyro_calib_pitch_denom) * uhid::PS5_GYRO_RES_PER_DEG_S * 5);
    this->_state->current_state.gyro[1] =
        htole16(rad2deg((y + uhid::gyro_calib_bias) / uhid::gyro_calib_yaw_denom) * uhid::PS5_GYRO_RES_PER_DEG_S * 5);
    this->_state->current_state.gyro[2] =
        htole16(rad2deg((z + uhid::gyro_calib_bias) / uhid::gyro_calib_roll_denom) * uhid::PS5_GYRO_RES_PER_DEG_S * 5);
    send_report(*this->_state);
    break;
  }
  }
}

void PS5Joypad::set_battery(PS5Joypad::BATTERY_STATE state, int percentage) {
  /*
   * Each unit of battery data corresponds to 10%
   * 0 = 0-9%, 1 = 10-19%, .. and 10 = 100%
   */
  this->_state->current_state.battery_charge = std::lround((percentage / 10));
  this->_state->current_state.battery_status = state;
  send_report(*this->_state);
}

void PS5Joypad::set_on_led(const std::function<void(int, int, int)> &callback) {
  this->_state->on_led = callback;
}

void PS5Joypad::place_finger(int finger_nr, uint16_t x, uint16_t y) {
  if (finger_nr <= 1) {
    this->_state->current_state.points[finger_nr].contact = 0;
    this->_state->current_state.points[finger_nr].id = this->_state->touch_points_ids[finger_nr] + 1;

    this->_state->current_state.points[finger_nr].x_lo = static_cast<uint8_t>(x & 0x00FF);
    this->_state->current_state.points[finger_nr].x_hi = static_cast<uint8_t>((x & 0xFF00) >> 8);

    this->_state->current_state.points[finger_nr].y_lo = static_cast<uint8_t>(y & 0x00FF);
    this->_state->current_state.points[finger_nr].y_hi = static_cast<uint8_t>((y & 0xFF00) >> 4);

    send_report(*this->_state);
  }
}

void PS5Joypad::release_finger(int finger_nr) {
  if (finger_nr <= 1) {
    this->_state->touch_points_ids[finger_nr]++;
    // if it goes above 0x7F we should reset it to 0
    if (this->_state->touch_points_ids[finger_nr] > 0x7F) {
      this->_state->touch_points_ids[finger_nr] = 0;
    }
    this->_state->current_state.points[finger_nr].contact = 1;
    send_report(*this->_state);
  }
}

} // namespace inputtino