/* mbedTLS icin entropi kaynagi: NaCl calisma zamaninin guvenli rastgele arayuzu. */
#include <stddef.h>
#include <nacl/nacl_random.h>
#include <mbedtls/entropy.h>

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	size_t got = 0;
	(void)data;
	*olen = 0;
	while(got < len)
	{
		size_t n = 0;
		if(nacl_secure_random(output + got, len - got, &n) != 0 || n == 0)
			return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
		got += n;
	}
	*olen = got;
	return 0;
}
