/*
 * RSA public key verification for the CarLife phone app's authentication response.
 * See md_authen.h for what this is for.
 *
 * Both steps of the check are libcrypto calls. The protocol value is decoded with
 * EVP_DecodeUpdate, then EVP_PKEY_verify_recover with RSA_PKCS1_PADDING does the public
 * exponentiation and strips the PKCS#1 v1.5 block in one call, which is the whole of the
 * check: it fails for any block whose padding does not parse and for any block that is not
 * below the modulus, so the recovered buffer is either the text the phone signed or nothing
 * at all. Only the public key is loaded, so no private material takes part.
 */
#include "md_authen.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <stdint.h>
#include <string.h>

/* The phone app's RSA-1024 public key, kept as the PEM it is distributed as
 * (reverse-carplaybox/carlife_app/keys/carlife_md_rsa_public.pem), so that it can be
 * compared with the file and with the report without decoding anything. The same key is
 * quoted in full as hex in CARLIFE_AUTH.md section 1.1 and can be inspected with
 *   openssl rsa -pubin -in carlife_md_rsa_public.pem -text -noout
 * The vehicle side library has no copy of it: this is the public half of the key the app
 * carries, taken out of the APK. */
static const char MD_PUBLIC_KEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQC86AUdyKOcMUB/iDMtHpFBOdLP\n"
    "WThDziP8t20sF0mRKlNBIAxlUZxQWIa/DEhJL9kx05+RyrFDN4B3d66UIURtia3G\n"
    "hWPoiBCbNornl73ynfFKk1BUDma3O8Qs9Z6YOCkXrGOFQBDfltfXs/w7CogkuS2s\n"
    "ZsBPOOMVom04I7KWPQIDAQAB\n"
    "-----END PUBLIC KEY-----\n";

/* --------------------------------------------------------------- encoding */

/* Base64 length of one block: four characters per three bytes, rounded up. */
#define MD_BASE64_SIZE (4 * ((MD_AUTHEN_SIGNATURE_SIZE + 2) / 3))

/* Decode the protocol's Base64 value. <out> must have room for MD_BASE64_SIZE bytes, which
 * is what one block can occupy before the padding is taken off; the number of bytes written
 * is returned, or -1.
 *
 * The value the app sends is android.util.Base64.NO_WRAP output. Decoding it is left to
 * libcrypto rather than being hand-rolled: EVP_DecodeUpdate is strict where it matters -- it
 * rejects data after the padding, and padding that does not close the last group -- and it
 * reports the length with the padding already taken off. It is more permissive than a
 * purpose-built decoder in skipping whitespace, which costs nothing here, because the caller
 * requires exactly one block and then a successful RSA check either way. */
static int base64_decode(const char* text, uint8_t* out)
{
    size_t length = strlen(text);
    /* A longer value cannot decode to a single block, and the decoder would then write past
     * the output; a shorter one cannot either, so it is not worth the call. */
    if (length == 0 || length > MD_BASE64_SIZE) {
        return -1;
    }

    EVP_ENCODE_CTX* context = EVP_ENCODE_CTX_new();
    if (context == NULL) {
        return -1;
    }
    EVP_DecodeInit(context);
    int written = 0;
    int update = EVP_DecodeUpdate(context, out, &written, (const unsigned char*)text, (int)length);
    int tail = 0;
    /* The final call reports whether the value ended on a group boundary. */
    int final = update < 0 ? -1 : EVP_DecodeFinal(context, out + written, &tail);
    EVP_ENCODE_CTX_free(context);

    if (update < 0 || final != 1) {
        return -1;
    }
    return written + tail;
}

/* ---------------------------------------------------------------- public */

int md_authen_recover(const char* encrypt_value, char* out, size_t out_cap)
{
    if (encrypt_value == NULL || out == NULL || out_cap == 0) {
        return -1;
    }

    uint8_t block[MD_BASE64_SIZE];
    if (base64_decode(encrypt_value, block) != MD_AUTHEN_SIGNATURE_SIZE) {
        return -1;
    }

    /* The key is read per call rather than cached: the exchange happens once per connection,
     * so the parsing cost does not matter, and a shared EVP_PKEY would need locking. */
    int result = -1;
    BIO* source = BIO_new_mem_buf(MD_PUBLIC_KEY_PEM, -1);
    EVP_PKEY* key = source == NULL ? NULL : PEM_read_bio_PUBKEY(source, NULL, NULL, NULL);
    EVP_PKEY_CTX* context = NULL;
    size_t plain_len = 0;
    uint8_t plain[MD_AUTHEN_SIGNATURE_SIZE];

    BIO_free(source);
    if (key == NULL) {
        goto done;
    }
    context = EVP_PKEY_CTX_new(key, NULL);
    if (context == NULL) {
        goto done;
    }
    if (EVP_PKEY_verify_recover_init(context) != 1) {
        goto done;
    }
    if (EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) != 1) {
        goto done;
    }

    plain_len = sizeof(plain);
    /* 1 = an RSA signature block was recovered, 0 = the block is not one, negative = the
     * operation itself failed (an input at or above the modulus, for instance). Every
     * outcome other than 1 means the same thing here: not an answer from the app. */
    if (EVP_PKEY_verify_recover(context, plain, &plain_len, block, MD_AUTHEN_SIGNATURE_SIZE) !=
        1) {
        goto done;
    }
    /* plain_len is at most one block, and MD_AUTHEN_MAX_MESSAGE is what a block of that size
     * can carry, so a caller sized after the protocol never gets here. */
    if (plain_len + 1 > out_cap) {
        goto done;
    }
    memcpy(out, plain, plain_len);
    out[plain_len] = '\0';
    result = (int)plain_len;

done:
    EVP_PKEY_CTX_free(context);
    EVP_PKEY_free(key);
    return result;
}
