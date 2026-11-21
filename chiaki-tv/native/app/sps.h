// chiaki-tv: H.264 dizi parametre kumesini (SPS) dusuk gecikme icin yeniden yazar.
//
// PS5 akisi 9 referans kare ve 9 karelik cozucu tamponu bildirir. Bu televizyonun donanim cozucusu
// tampon dolana kadar resim vermez (olculen: ~230 ms). Tampon boyutu kucuk bildirilirse cozucu
// kareleri hemen cikarir. Pepper'a bagimliligi yoktur; masaustunde de derlenip sinanabilir.
#ifndef CHIAKI_TV_SPS_H
#define CHIAKI_TV_SPS_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace sps {

struct Patch {
  int max_refs;  // max_num_ref_frames ust siniri (0 = dokunma)
  int max_dpb;   // max_dec_frame_buffering (0 = dokunma); max_num_reorder_frames her zaman 0 yapilir
  int level;     // level_idc (0 = dokunma), ornegin 31
  Patch() : max_refs(0), max_dpb(0), level(0) {}
  bool active() const { return max_refs > 0 || max_dpb > 0 || level > 0; }
};

// --- "bos kare" ekleme (cozucunun ic sirasini hizlandirmak icin) ---
// Televizyonun cozucusu bir resmi, ardindan birkac resim daha verildikten sonra cikarir. Her gercek karenin
// arkasina, onun birebir kopyasi olan (tum makro bloklari "atla") referans olmayan kucuk bir kare eklenirse
// sira iki kat hizli ilerler. Bu kareler ayri bir PPS (kimlik 1, CAVLC) kullanir.
struct Info {
  bool valid;
  int log2_max_frame_num;  // frame_num alaninin bit sayisi
  int mbs;                 // resimdeki makro blok sayisi
  Info() : valid(false), log2_max_frame_num(0), mbs(0) {}
};
// Akistaki ilk SPS'ten bos kare icin gerekenleri okur. Desteklenmeyen akista (alanli kodlama, POC turu 2
// degil) valid=false kalir.
bool ReadInfo(const uint8_t* data, size_t size, Info* info);
// Bos karelerin kullandigi PPS (baslangic kodu dahil).
void BuildSkipPps(std::vector<uint8_t>* out);
// Bir erisim biriminin (karenin) ilk diliminden frame_num okur. Kare referanssa *is_ref true olur.
// Dilim bulunamazsa -1.
int FrameNum(const uint8_t* data, size_t size, const Info& info, bool* is_ref);
// frame_num degeri verilen bos kareyi uretir (baslangic kodu dahil).
void BuildSkipFrame(const Info& info, int frame_num, std::vector<uint8_t>* out);

// Annex-B akisindaki (baslangic kodlu) her SPS birimini yeniden yazar; diger birimler aynen kopyalanir.
// Ayristirilamayan SPS'e dokunulmaz. Hic SPS degismediyse false doner (out yine girdinin kopyasidir).
bool PatchStream(const uint8_t* data, size_t size, const Patch& patch, std::vector<uint8_t>* out);

}  // namespace sps

#endif
