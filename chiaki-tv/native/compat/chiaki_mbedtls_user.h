/* mbedTLS 2.28 varsayilan ayarlarinin ustune NaCl (newlib) icin duzeltmeler.
 * Bu dosya mbedTLS'i kullanan HER derleme biriminde ayni olmali: THREADING yapilarin boyutunu degistirir. */
#ifndef CHIAKI_MBEDTLS_USER_H
#define CHIAKI_MBEDTLS_USER_H

/* NaCl'de BSD soketleri nacl_io'dan gelir; mbedTLS'in kendi ag ve zamanlayici katmani kullanilmaz. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_HAVEGE_C

/* Dosyadan sertifika okuma acik: kok sertifika paketi nacl_io bellek dosya sisteminde durur. */
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* Elle yazilmis ARM cevirici kodu (bn_mul.h) bellek erisimlerini maskelemez: NaCl dogrulayicisi
 * modulu reddeder. Buyuk sayi carpimi C ile derlensin; derleyici korumali alan kurallarini uygular. */
#undef MBEDTLS_HAVE_ASM

/* x86'ya ozel hizlandirmalar. */
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_PADLOCK_C

/* Rastgelelik: /dev/urandom yerine NaCl'in guvenli rastgele kaynagi (entropy_nacl.c). */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* curl ve chiaki farkli is parcaciklarindan cagirir. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

#endif
