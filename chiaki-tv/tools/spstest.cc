// sps.cc'nin masaustu sinamasi.
//   spstest patch <girdi.h264> <cikti.h264> <max_refs> <max_dpb> <level>   SPS'leri yeniden yazar
//   spstest skip  <girdi.h264> <cikti.h264> <max_refs> <max_dpb>           ayrica her karenin arkasina bos kare ekler
// "skip" girdisi kare sinirlarini bilmek icin birimleri 4 baytlik uzunlukla ister (tvdebug capture.bin).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "../native/app/sps.h"

static std::vector<uint8_t> ReadAll(const char* path) {
  std::vector<uint8_t> in;
  FILE* f = fopen(path, "rb");
  if (!f) return in;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) in.insert(in.end(), buf, buf + n);
  fclose(f);
  return in;
}

int main(int argc, char** argv) {
  if (argc < 6) return 2;
  std::vector<uint8_t> in = ReadAll(argv[2]), out;
  sps::Patch patch;
  patch.max_refs = atoi(argv[4]);
  patch.max_dpb = atoi(argv[5]);
  if (!strcmp(argv[1], "patch")) {
    patch.level = argc > 6 ? atoi(argv[6]) : 0;
    bool changed = sps::PatchStream(in.data(), in.size(), patch, &out);
    printf("%s: %zu -> %zu bayt\n", changed ? "degisti" : "DEGISMEDI", in.size(), out.size());
  } else {
    sps::Info info;
    std::vector<uint8_t> pps, skip, unit;
    sps::BuildSkipPps(&pps);
    int frames = 0, skips = 0;
    for (size_t pos = 0; pos + 4 <= in.size();) {
      size_t n = (in[pos] << 24) | (in[pos + 1] << 16) | (in[pos + 2] << 8) | in[pos + 3];
      const uint8_t* data = in.data() + pos + 4;
      pos += 4 + n;
      sps::PatchStream(data, n, patch, &unit);
      out.insert(out.end(), unit.begin(), unit.end());
      sps::Info fresh;
      if (sps::ReadInfo(data, n, &fresh)) {  // baslik birimi: bos kare PPS'ini ekle
        info = fresh;
        out.insert(out.end(), pps.begin(), pps.end());
        continue;
      }
      bool is_ref = false;
      int frame_num = sps::FrameNum(data, n, info, &is_ref);
      frames++;
      if (frame_num < 0 || !is_ref) continue;
      sps::BuildSkipFrame(info, frame_num + 1, &skip);
      out.insert(out.end(), skip.begin(), skip.end());
      skips++;
    }
    printf("%d kare, %d bos kare eklendi (bos kare %zu bayt, PPS %zu bayt), frame_num %d bit, %d makro blok\n", frames, skips,
           skip.size(), pps.size(), info.log2_max_frame_num, info.mbs);
  }
  FILE* f = fopen(argv[3], "wb");
  fwrite(out.data(), 1, out.size(), f);
  fclose(f);
  return 0;
}
