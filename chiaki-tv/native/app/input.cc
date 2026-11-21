#include "input.h"

#include <string.h>

#include "ppapi/cpp/module.h"

#include "i18n.h"
#include "util.h"

namespace {

const int64_t kNetPadStaleMs = 400;
const uint32_t kMenuCombo = CHIAKI_CONTROLLER_BUTTON_L1 | CHIAKI_CONTROLLER_BUTTON_R1 | CHIAKI_CONTROLLER_BUTTON_L3 |
                            CHIAKI_CONTROLLER_BUTTON_R3;

int16_t MaxAbs(int16_t a, int16_t b) {
  int aa = a < 0 ? -a : a, bb = b < 0 ? -b : b;
  return aa >= bb ? a : b;
}

// W3C "standard" oyun kolu duzeni -> chiaki dugmeleri. 6 ve 7 (L2/R2) analog oldugu icin ayri islenir.
const uint32_t kButtonMap[18] = {
    CHIAKI_CONTROLLER_BUTTON_CROSS,     // 0  alt   (X)
    CHIAKI_CONTROLLER_BUTTON_MOON,      // 1  sag   (daire)
    CHIAKI_CONTROLLER_BUTTON_BOX,       // 2  sol   (kare)
    CHIAKI_CONTROLLER_BUTTON_PYRAMID,   // 3  ust   (ucgen)
    CHIAKI_CONTROLLER_BUTTON_L1,        // 4
    CHIAKI_CONTROLLER_BUTTON_R1,        // 5
    0,                                  // 6  L2 (analog)
    0,                                  // 7  R2 (analog)
    CHIAKI_CONTROLLER_BUTTON_SHARE,     // 8
    CHIAKI_CONTROLLER_BUTTON_OPTIONS,   // 9
    CHIAKI_CONTROLLER_BUTTON_L3,        // 10
    CHIAKI_CONTROLLER_BUTTON_R3,        // 11
    CHIAKI_CONTROLLER_BUTTON_DPAD_UP,   // 12
    CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN, // 13
    CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT, // 14
    CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT,// 15
    CHIAKI_CONTROLLER_BUTTON_PS,        // 16
    CHIAKI_CONTROLLER_BUTTON_TOUCHPAD,  // 17
};

int16_t Axis(float v) {
  if (v > -0.08f && v < 0.08f) v = 0.0f;  // olu bolge: eskimis cubuklarin kaymasini yut
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  return static_cast<int16_t>(v * 32767.0f);
}

uint8_t Trigger(float v) {
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  return static_cast<uint8_t>(v * 255.0f);
}

uint32_t RemoteButton(const std::string& key) {
  if (key == "up") return CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
  if (key == "down") return CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
  if (key == "left") return CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
  if (key == "right") return CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
  if (key == "ok") return CHIAKI_CONTROLLER_BUTTON_CROSS;
  if (key == "back") return CHIAKI_CONTROLLER_BUTTON_MOON;
  if (key == "red") return CHIAKI_CONTROLLER_BUTTON_PS;
  if (key == "green") return CHIAKI_CONTROLLER_BUTTON_OPTIONS;
  if (key == "yellow") return CHIAKI_CONTROLLER_BUTTON_PYRAMID;
  if (key == "blue") return CHIAKI_CONTROLLER_BUTTON_BOX;
  return 0;
}

}  // namespace

Input::Input(pp::Instance* instance)
    : instance_(instance),
      gamepad_if_(static_cast<const PPB_Gamepad*>(pp::Module::Get()->GetBrowserInterface(PPB_GAMEPAD_INTERFACE))),
      remote_buttons_(0),
      has_last_(false),
      combo_down_(false),
      menu_toggle_(false),
      hold_combo_(false),
      captured_(false),
      nav_prev_(0),
      net_ms_(0) {
  chiaki_controller_state_set_idle(&last_);
  chiaki_controller_state_set_idle(&net_);
}

void Input::SetNetPad(uint32_t buttons, uint8_t l2, uint8_t r2, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
  net_.buttons = buttons & 0xffff;  // yalnizca sayisal dugmeler; L2/R2 analog alanlardan gelir
  net_.l2_state = l2;
  net_.r2_state = r2;
  net_.left_x = lx;
  net_.left_y = ly;
  net_.right_x = rx;
  net_.right_y = ry;
  net_ms_ = util::NowMs();
}

void Input::ClearNetPad() {
  chiaki_controller_state_set_idle(&net_);
  net_ms_ = 0;
  net_name_.clear();
}

void Input::Reset() {
  remote_buttons_ = 0;
  has_last_ = false;
  combo_down_ = menu_toggle_ = hold_combo_ = captured_ = false;
  nav_prev_ = 0;
  nav_.clear();
}

bool Input::TakeMenuToggle() {
  bool v = menu_toggle_;
  menu_toggle_ = false;
  return v;
}

void Input::SetCaptured(bool on) {
  captured_ = on;
  nav_prev_ = 0xffffffff;  // acilis/kapanis aninda basili tuslar yeni basis sayilmasin
  nav_.clear();
}

std::vector<std::string> Input::TakeNav() {
  std::vector<std::string> out;
  out.swap(nav_);
  return out;
}

void Input::SetRemoteKey(const std::string& key, bool down) {
  uint32_t bit = RemoteButton(key);
  if (down) remote_buttons_ |= bit;
  else remote_buttons_ &= ~bit;
}

bool Input::Sample(ChiakiControllerState* out) {
  ChiakiControllerState state;
  chiaki_controller_state_set_idle(&state);
  gamepad_name_.clear();
  if (gamepad_if_) {
    PP_GamepadsSampleData data;
    memset(&data, 0, sizeof(data));
    gamepad_if_->Sample(instance_->pp_instance(), &data);
    for (uint32_t i = 0; i < data.length && i < 4; i++) {
      const PP_GamepadSampleData& pad = data.items[i];
      if (!pad.connected) continue;
      for (size_t c = 0; c < sizeof(pad.id) / sizeof(pad.id[0]) && pad.id[c]; c++)
        gamepad_name_ += pad.id[c] < 128 ? static_cast<char>(pad.id[c]) : '?';
      if (gamepad_name_.empty()) gamepad_name_ = L("oyun kolu");
      uint32_t count = pad.buttons_length < 18 ? pad.buttons_length : 18;
      for (uint32_t b = 0; b < count; b++)
        if (kButtonMap[b] && pad.buttons[b] > 0.5f) state.buttons |= kButtonMap[b];
      if (pad.buttons_length > 6) state.l2_state = Trigger(pad.buttons[6]);
      if (pad.buttons_length > 7) state.r2_state = Trigger(pad.buttons[7]);
      if (pad.axes_length >= 2) {
        state.left_x = Axis(pad.axes[0]);
        state.left_y = Axis(pad.axes[1]);
      }
      if (pad.axes_length >= 4) {
        state.right_x = Axis(pad.axes[2]);
        state.right_y = Axis(pad.axes[3]);
      }
      break;  // ilk bagli kol yeter
    }
  }
  state.buttons |= remote_buttons_;
  if (net_ms_ && util::NowMs() - net_ms_ <= kNetPadStaleMs) {
    state.buttons |= net_.buttons;
    if (net_.l2_state > state.l2_state) state.l2_state = net_.l2_state;
    if (net_.r2_state > state.r2_state) state.r2_state = net_.r2_state;
    state.left_x = MaxAbs(state.left_x, net_.left_x);
    state.left_y = MaxAbs(state.left_y, net_.left_y);
    state.right_x = MaxAbs(state.right_x, net_.right_x);
    state.right_y = MaxAbs(state.right_y, net_.right_y);
  }
  if (gamepad_name_.empty() && net_ms_) gamepad_name_ = net_name_.empty() ? L("Tarayıcı kolu") : net_name_;
  // --- yayin menusu kisayolu: dort tus birlikte ---
  bool combo = (state.buttons & kMenuCombo) == kMenuCombo;
  if (combo && !combo_down_) menu_toggle_ = true;
  combo_down_ = combo;
  if (combo) hold_combo_ = true;
  if (hold_combo_) {  // kisayol tuslari tamamen birakilana kadar konsola gitmesin
    if ((state.buttons & kMenuCombo) == 0) hold_combo_ = false;
    state.buttons &= ~kMenuCombo;
  }
  if (captured_) {
    // Menu acik: basis kenarlarini gezinme olaylarina cevir, konsola bos durum gonder.
    uint32_t nav = state.buttons;
    if (state.left_y < -16000) nav |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
    if (state.left_y > 16000) nav |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
    if (state.left_x < -16000) nav |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
    if (state.left_x > 16000) nav |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
    uint32_t pressed = nav & ~nav_prev_;
    nav_prev_ = nav;
    if (pressed & CHIAKI_CONTROLLER_BUTTON_DPAD_UP) nav_.push_back("up");
    if (pressed & CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN) nav_.push_back("down");
    if (pressed & CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT) nav_.push_back("left");
    if (pressed & CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT) nav_.push_back("right");
    if (pressed & CHIAKI_CONTROLLER_BUTTON_CROSS) nav_.push_back("ok");
    if (pressed & CHIAKI_CONTROLLER_BUTTON_MOON) nav_.push_back("back");
    chiaki_controller_state_set_idle(&state);
  }
  bool changed = !has_last_ || state.buttons != last_.buttons || state.l2_state != last_.l2_state ||
                 state.r2_state != last_.r2_state || state.left_x != last_.left_x || state.left_y != last_.left_y ||
                 state.right_x != last_.right_x || state.right_y != last_.right_y;
  last_ = state;
  has_last_ = true;
  *out = state;
  return changed;
}
