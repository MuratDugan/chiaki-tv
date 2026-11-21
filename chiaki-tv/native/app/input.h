// chiaki-tv: oyun kolu (Pepper Gamepad) ve TV kumandasi tuslarini chiaki kumanda durumuna cevirir.
#ifndef CHIAKI_TV_INPUT_H
#define CHIAKI_TV_INPUT_H

#include <stdint.h>

#include <string>
#include <vector>

#include <chiaki/controller.h>

#include "ppapi/c/ppb_gamepad.h"
#include "ppapi/cpp/instance.h"

class Input {
 public:
  explicit Input(pp::Instance* instance);

  // Ana is parcaciginda: guncel durumu okur. Onceki cagridan beri degistiyse true doner.
  bool Sample(ChiakiControllerState* out);

  // TV kumandasi tusu (JS'ten): "up", "down", "left", "right", "ok", "back", "red", "green", "yellow", "blue".
  void SetRemoteKey(const std::string& key, bool down);
  void Reset();

  // Agdan gelen kol (telefon/bilgisayar tarayicisindaki kol sayfasi). Durum kNetPadStaleMs icinde
  // yenilenmezse bosa donulur: baglanti koparsa ya da telefon uyursa tus basili kalmaz.
  void SetNetPad(uint32_t buttons, uint8_t l2, uint8_t r2, int16_t lx, int16_t ly, int16_t rx, int16_t ry);
  void SetNetPadName(const std::string& name) { net_name_ = name; }
  void ClearNetPad();

  // Yayin menusu (chiaki-ng ile ayni kisayol): L1 + R1 + L3 + R3 birlikte basilinca bir kez true doner.
  bool TakeMenuToggle();
  // Menu acikken kol konsola gitmez (Sample bos durum verir); yon tuslari, carpi ve daire menuyu gezdirir.
  void SetCaptured(bool on);
  // Menu acikken basilan gezinme tuslari: "up", "down", "left", "right", "ok", "back".
  std::vector<std::string> TakeNav();

  // Bagli ilk oyun kolunun adi; yoksa bos.
  const std::string& gamepad_name() const { return gamepad_name_; }

 private:
  pp::Instance* instance_;
  const PPB_Gamepad* gamepad_if_;
  uint32_t remote_buttons_;
  ChiakiControllerState last_;
  bool has_last_;
  std::string gamepad_name_;
  ChiakiControllerState net_;
  bool combo_down_, menu_toggle_, hold_combo_, captured_;
  uint32_t nav_prev_;
  std::vector<std::string> nav_;
  int64_t net_ms_;  // son ag kolu guncellemesi (0 = yok)
  std::string net_name_;
};

#endif
