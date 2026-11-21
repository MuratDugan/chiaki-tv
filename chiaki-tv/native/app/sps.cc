#include "sps.h"

namespace sps {

namespace {

struct BitReader {
  const std::vector<uint8_t>& d;
  size_t pos;  // bit
  bool bad;
  explicit BitReader(const std::vector<uint8_t>& data) : d(data), pos(0), bad(false) {}
  uint32_t Bits(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
      if (pos >= d.size() * 8) {
        bad = true;
        return 0;
      }
      v = (v << 1) | ((d[pos >> 3] >> (7 - (pos & 7))) & 1);
      pos++;
    }
    return v;
  }
  uint32_t Ue() {
    int zeros = 0;
    while (!bad && Bits(1) == 0)
      if (++zeros > 31) bad = true;
    if (bad) return 0;
    return zeros ? ((1u << zeros) - 1 + Bits(zeros)) : 0;
  }
};

struct BitWriter {
  std::vector<uint8_t> d;
  int fill;  // son bayttaki dolu bit sayisi (0..7)
  BitWriter() : fill(0) {}
  void Bits(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
      if (fill == 0) d.push_back(0);
      d.back() |= static_cast<uint8_t>(((v >> i) & 1) << (7 - fill));
      fill = (fill + 1) & 7;
    }
  }
  void Ue(uint32_t v) {
    uint32_t x = v + 1;
    int len = 0;
    for (uint32_t t = x; t > 1; t >>= 1) len++;
    Bits(0, len);
    Bits(x, len + 1);
  }
};

// Okuyup aynen yazan yardimcilar.
struct Copier {
  BitReader& r;
  BitWriter& w;
  Copier(BitReader& reader, BitWriter& writer) : r(reader), w(writer) {}
  uint32_t U(int n) {
    uint32_t v = r.Bits(n);
    w.Bits(v, n);
    return v;
  }
  uint32_t Ue() {
    uint32_t v = r.Ue();
    w.Ue(v);
    return v;
  }
  void Hrd() {
    uint32_t count = Ue() + 1;  // cpb_cnt_minus1
    U(4);
    U(4);
    for (uint32_t i = 0; i < count && i < 32 && !r.bad; i++) {
      Ue();
      Ue();
      U(1);
    }
    U(5);
    U(5);
    U(5);
    U(5);
  }
};

bool HighProfile(uint32_t p) {
  return p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 ||
         p == 138 || p == 139 || p == 134 || p == 135;
}

// nal: baslangic kodu haric, NAL basligi dahil, taklit onleme baytlari (00 00 03) icinde.
bool PatchSps(const uint8_t* nal, size_t size, const Patch& patch, std::vector<uint8_t>* out) {
  std::vector<uint8_t> rbsp;
  rbsp.reserve(size);
  for (size_t i = 0; i < size; i++) {
    if (i >= 2 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0) continue;  // taklit onleme bayti
    rbsp.push_back(nal[i]);
  }
  BitReader r(rbsp);
  BitWriter w;
  Copier c(r, w);
  c.U(8);  // NAL basligi
  uint32_t profile = c.U(8);
  c.U(8);  // constraint_set bayraklari
  uint32_t level = r.Bits(8);
  w.Bits(patch.level > 0 ? static_cast<uint32_t>(patch.level) : level, 8);
  c.Ue();  // seq_parameter_set_id
  if (HighProfile(profile)) {
    uint32_t chroma = c.Ue();
    if (chroma == 3) c.U(1);
    c.Ue();
    c.Ue();
    c.U(1);
    if (c.U(1)) return false;  // olcekleme matrisleri: bu akislarda yok, ayristirmaya deger degil
  }
  c.Ue();  // log2_max_frame_num_minus4
  uint32_t poc_type = c.Ue();
  if (poc_type == 0) {
    c.Ue();
  } else if (poc_type == 1) {
    c.U(1);
    c.Ue();  // se(v) de ayni bit dizilimiyle kopyalanir
    c.Ue();
    uint32_t n = c.Ue();
    for (uint32_t i = 0; i < n && i < 256 && !r.bad; i++) c.Ue();
  }
  uint32_t refs = r.Ue();
  if (patch.max_refs > 0 && refs > static_cast<uint32_t>(patch.max_refs)) refs = static_cast<uint32_t>(patch.max_refs);
  w.Ue(refs);
  c.U(1);  // gaps_in_frame_num_value_allowed_flag
  c.Ue();  // genislik
  c.Ue();  // yukseklik
  if (!c.U(1)) c.U(1);  // frame_mbs_only_flag, mb_adaptive_frame_field_flag
  c.U(1);               // direct_8x8_inference_flag
  if (c.U(1)) {         // kirpma
    c.Ue();
    c.Ue();
    c.Ue();
    c.Ue();
  }
  uint32_t dpb = patch.max_dpb > 0 ? static_cast<uint32_t>(patch.max_dpb) : 0;
  bool has_vui = r.Bits(1) != 0;
  w.Bits(1, 1);  // VUI her durumda yazilir: akis kisitlamasi orada
  bool had_restriction = false;
  if (has_vui) {
    if (c.U(1) && c.U(8) == 255) c.U(32);  // en-boy orani
    if (c.U(1)) c.U(1);                    // overscan
    if (c.U(1)) {                          // video sinyal turu
      c.U(4);
      if (c.U(1)) c.U(24);
    }
    if (c.U(1)) {  // renk ornekleme konumu
      c.Ue();
      c.Ue();
    }
    if (c.U(1)) {  // zamanlama
      c.U(32);
      c.U(32);
      c.U(1);
    }
    bool nal_hrd = c.U(1) != 0;
    if (nal_hrd) c.Hrd();
    bool vcl_hrd = c.U(1) != 0;
    if (vcl_hrd) c.Hrd();
    if (nal_hrd || vcl_hrd) c.U(1);  // low_delay_hrd_flag
    c.U(1);                          // pic_struct_present_flag
    had_restriction = r.Bits(1) != 0;
    w.Bits(1, 1);
    if (had_restriction) {
      c.U(1);
      c.Ue();
      c.Ue();
      c.Ue();
      c.Ue();
      r.Ue();  // max_num_reorder_frames
      uint32_t old_dpb = r.Ue();
      w.Ue(0);
      w.Ue(dpb ? dpb : old_dpb);
    }
  } else {
    // VUI yoktu: yalnizca akis kisitlamasi iceren en kucuk VUI. Sekiz bayrak (en-boy, overscan, sinyal,
    // renk konumu, zamanlama, nal_hrd, vcl_hrd, pic_struct) sifir, ardindan bitstream_restriction_flag = 1.
    w.Bits(0, 8);
    w.Bits(1, 1);
  }
  if (r.bad) return false;
  if (!had_restriction) {
    w.Bits(1, 1);  // motion_vectors_over_pic_boundaries_flag
    w.Ue(2);       // max_bytes_per_pic_denom (varsayilan)
    w.Ue(1);       // max_bits_per_mb_denom (varsayilan)
    w.Ue(16);      // log2_max_mv_length_horizontal
    w.Ue(16);      // log2_max_mv_length_vertical
    w.Ue(0);       // max_num_reorder_frames
    w.Ue(dpb ? dpb : (refs ? refs : 1));
  }
  w.Bits(1, 1);  // rbsp_stop_one_bit; kalan bitler sifir
  // Taklit onleme baytlarini yeniden ekle.
  out->clear();
  int zeros = 0;
  for (size_t i = 0; i < w.d.size(); i++) {
    uint8_t b = w.d[i];
    if (zeros >= 2 && b <= 3) {
      out->push_back(3);
      zeros = 0;
    }
    out->push_back(b);
    zeros = b == 0 ? zeros + 1 : 0;
  }
  return true;
}

}  // namespace

// Ilk NAL birimini (istenen turde) bulur: baslik baytinin konumu, yoksa size.
static size_t FindNal(const uint8_t* data, size_t size, bool (*want)(int type), size_t* end) {
  size_t found = size;
  for (size_t i = 0; i + 3 < size; i++) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      if (found != size) {
        *end = (i > 0 && data[i - 1] == 0) ? i - 1 : i;
        return found;
      }
      if (want(data[i + 3] & 0x1f)) found = i + 3;
      i += 2;
    }
  }
  *end = size;
  return found;
}

static bool IsSps(int type) { return type == 7; }
static bool IsSlice(int type) { return type == 1 || type == 5; }

static void Unescape(const uint8_t* nal, size_t size, size_t limit, std::vector<uint8_t>* rbsp) {
  for (size_t i = 0; i < size && rbsp->size() < limit; i++) {
    if (i >= 2 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0) continue;
    rbsp->push_back(nal[i]);
  }
}

static void Escape(const std::vector<uint8_t>& rbsp, std::vector<uint8_t>* out) {
  int zeros = 0;
  for (size_t i = 0; i < rbsp.size(); i++) {
    uint8_t b = rbsp[i];
    if (zeros >= 2 && b <= 3) {
      out->push_back(3);
      zeros = 0;
    }
    out->push_back(b);
    zeros = b == 0 ? zeros + 1 : 0;
  }
}

bool ReadInfo(const uint8_t* data, size_t size, Info* info) {
  *info = Info();
  size_t end = 0, at = FindNal(data, size, IsSps, &end);
  if (at >= size) return false;
  std::vector<uint8_t> rbsp;
  Unescape(data + at, end - at, 256, &rbsp);
  BitReader r(rbsp);
  r.Bits(8);
  uint32_t profile = r.Bits(8);
  r.Bits(16);
  r.Ue();
  if (HighProfile(profile)) {
    uint32_t chroma = r.Ue();
    if (chroma == 3) r.Bits(1);
    r.Ue();
    r.Ue();
    r.Bits(1);
    if (r.Bits(1)) return false;
  }
  uint32_t log2_frame_num = r.Ue() + 4;
  uint32_t poc_type = r.Ue();
  if (poc_type != 2) return false;  // bos karenin dilim basligi yalnizca POC turu 2 icin yaziliyor
  r.Ue();                           // max_num_ref_frames
  r.Bits(1);
  uint32_t width_mbs = r.Ue() + 1, height_units = r.Ue() + 1;
  bool frame_mbs_only = r.Bits(1) != 0;
  if (r.bad || !frame_mbs_only || log2_frame_num > 16 || width_mbs > 256 || height_units > 256) return false;
  info->valid = true;
  info->log2_max_frame_num = static_cast<int>(log2_frame_num);
  info->mbs = static_cast<int>(width_mbs * height_units);
  return true;
}

void BuildSkipPps(std::vector<uint8_t>* out) {
  BitWriter w;
  w.Bits(0x68, 8);  // nal_ref_idc 3, tur 8 (PPS)
  w.Ue(1);          // pic_parameter_set_id
  w.Ue(0);          // seq_parameter_set_id
  w.Bits(0, 1);     // entropy_coding_mode_flag: CAVLC
  w.Bits(0, 1);     // bottom_field_pic_order_in_frame_present_flag
  w.Ue(0);          // num_slice_groups_minus1
  w.Ue(0);          // num_ref_idx_l0_default_active_minus1
  w.Ue(0);          // num_ref_idx_l1_default_active_minus1
  w.Bits(0, 1);     // weighted_pred_flag
  w.Bits(0, 2);     // weighted_bipred_idc
  w.Ue(0);          // pic_init_qp_minus26 (se 0)
  w.Ue(0);          // pic_init_qs_minus26
  w.Ue(0);          // chroma_qp_index_offset
  w.Bits(0, 1);     // deblocking_filter_control_present_flag
  w.Bits(0, 1);     // constrained_intra_pred_flag
  w.Bits(0, 1);     // redundant_pic_cnt_present_flag
  w.Bits(1, 1);     // rbsp_stop_one_bit
  out->clear();
  const uint8_t start[4] = {0, 0, 0, 1};
  out->insert(out->end(), start, start + 4);
  Escape(w.d, out);
}

int FrameNum(const uint8_t* data, size_t size, const Info& info, bool* is_ref) {
  size_t end = 0, at = FindNal(data, size, IsSlice, &end);
  if (at >= size || !info.valid) return -1;
  *is_ref = (data[at] & 0x60) != 0;
  std::vector<uint8_t> rbsp;
  Unescape(data + at, end - at, 24, &rbsp);
  BitReader r(rbsp);
  r.Bits(8);
  r.Ue();  // first_mb_in_slice
  r.Ue();  // slice_type
  r.Ue();  // pic_parameter_set_id
  uint32_t frame_num = r.Bits(info.log2_max_frame_num);
  return r.bad ? -1 : static_cast<int>(frame_num);
}

void BuildSkipFrame(const Info& info, int frame_num, std::vector<uint8_t>* out) {
  BitWriter w;
  w.Bits(0x01, 8);  // nal_ref_idc 0 (referans degil), tur 1 (IDR olmayan dilim)
  w.Ue(0);          // first_mb_in_slice
  w.Ue(5);          // slice_type: P (resimdeki tum dilimler)
  w.Ue(1);          // pic_parameter_set_id: bos kare PPS'i
  w.Bits(static_cast<uint32_t>(frame_num) & ((1u << info.log2_max_frame_num) - 1), info.log2_max_frame_num);
  w.Bits(0, 1);     // num_ref_idx_active_override_flag
  w.Bits(0, 1);     // ref_pic_list_modification_flag_l0
  w.Ue(0);          // slice_qp_delta (se 0)
  w.Ue(static_cast<uint32_t>(info.mbs));  // mb_skip_run: butun makro bloklar atlanir
  w.Bits(1, 1);     // rbsp_stop_one_bit
  out->clear();
  const uint8_t start[4] = {0, 0, 0, 1};
  out->insert(out->end(), start, start + 4);
  Escape(w.d, out);
}

bool PatchStream(const uint8_t* data, size_t size, const Patch& patch, std::vector<uint8_t>* out) {
  out->clear();
  bool changed = false;
  if (!patch.active()) {
    out->assign(data, data + size);
    return false;
  }
  // Baslangic kodlarini bul: (konum, uzunluk)
  std::vector<std::pair<size_t, size_t> > starts;
  for (size_t i = 0; i + 2 < size; i++) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      size_t begin = (i > 0 && data[i - 1] == 0) ? i - 1 : i;
      starts.push_back(std::make_pair(begin, i + 3 - begin));
      i += 2;
    }
  }
  if (starts.empty()) {
    out->assign(data, data + size);
    return false;
  }
  out->insert(out->end(), data, data + starts[0].first);
  for (size_t n = 0; n < starts.size(); n++) {
    size_t nal_begin = starts[n].first + starts[n].second;
    size_t nal_end = n + 1 < starts.size() ? starts[n + 1].first : size;
    out->insert(out->end(), data + starts[n].first, data + nal_begin);  // baslangic kodu
    std::vector<uint8_t> patched;
    if (nal_end > nal_begin && (data[nal_begin] & 0x1f) == 7 &&
        PatchSps(data + nal_begin, nal_end - nal_begin, patch, &patched)) {
      out->insert(out->end(), patched.begin(), patched.end());
      changed = true;
    } else {
      out->insert(out->end(), data + nal_begin, data + nal_end);
    }
  }
  return changed;
}

}  // namespace sps
