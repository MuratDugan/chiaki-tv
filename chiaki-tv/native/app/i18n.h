// chiaki-tv: modulun kullaniciya gosterdigi metinlerin dili. Kaynak kodda metinler Turkce yazilir ve
// L("...") ile sarilir; dil Ingilizce ise (varsayilan) i18n.cc'deki tablodan karsiligi dondurulur.
// Tabloda olmayan metin oldugu gibi kalir.
#ifndef CHIAKI_TV_I18N_H
#define CHIAKI_TV_I18N_H

#include <string>

namespace i18n {
void SetLang(const std::string& lang);  // "en" ya da "tr"; baska deger "en" sayilir
const char* Lang();
const char* Translate(const char* tr);
}  // namespace i18n

inline const char* L(const char* tr) { return i18n::Translate(tr); }

#endif
