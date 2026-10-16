/*
 * RSA public key verification for the CarLife phone app's authentication response.
 * See md_authen.h for what this is for.
 *
 * The arithmetic is a straight binary implementation on 32 bit limbs. The exponent is
 * 65537, so a verification costs 18 modular multiplications, and each of those is done
 * shift-and-add with a reduction per step: about 18k additions of a 1024 bit value, which
 * is on the order of a millisecond and runs once per connection. A full width product
 * followed by a division would be faster and much easier to get wrong, so it is not used.
 * 32 bit limbs rather than 64 bit ones keep this free of __int128 and portable to 32 bit
 * targets.
 */
#include "md_authen.h"

#include <stdint.h>
#include <string.h>

/* Modulus of the phone app's RSA-1024 public key, most significant byte first, with the
 * exponent in MD_EXPONENT. Regenerate and compare against the report with
 *   openssl rsa -pubin -in carlife_md_rsa_public.pem -text -noout
 * (the PEM is in reverse-carplaybox/carlife_app/keys/). The value is also quoted in full
 * as hex in CARLIFE_AUTH.md section 1.1. */
static const uint8_t MD_MODULUS[MD_AUTHEN_SIGNATURE_SIZE] = {
    0xbc, 0xe8, 0x05, 0x1d, 0xc8, 0xa3, 0x9c, 0x31,
    0x40, 0x7f, 0x88, 0x33, 0x2d, 0x1e, 0x91, 0x41,
    0x39, 0xd2, 0xcf, 0x59, 0x38, 0x43, 0xce, 0x23,
    0xfc, 0xb7, 0x6d, 0x2c, 0x17, 0x49, 0x91, 0x2a,
    0x53, 0x41, 0x20, 0x0c, 0x65, 0x51, 0x9c, 0x50,
    0x58, 0x86, 0xbf, 0x0c, 0x48, 0x49, 0x2f, 0xd9,
    0x31, 0xd3, 0x9f, 0x91, 0xca, 0xb1, 0x43, 0x37,
    0x80, 0x77, 0x77, 0xae, 0x94, 0x21, 0x44, 0x6d,
    0x89, 0xad, 0xc6, 0x85, 0x63, 0xe8, 0x88, 0x10,
    0x9b, 0x36, 0x8a, 0xe7, 0x97, 0xbd, 0xf2, 0x9d,
    0xf1, 0x4a, 0x93, 0x50, 0x54, 0x0e, 0x66, 0xb7,
    0x3b, 0xc4, 0x2c, 0xf5, 0x9e, 0x98, 0x38, 0x29,
    0x17, 0xac, 0x63, 0x85, 0x40, 0x10, 0xdf, 0x96,
    0xd7, 0xd7, 0xb3, 0xfc, 0x3b, 0x0a, 0x88, 0x24,
    0xb9, 0x2d, 0xac, 0x66, 0xc0, 0x4f, 0x38, 0xe3,
    0x15, 0xa2, 0x6d, 0x38, 0x23, 0xb2, 0x96, 0x3d,
};

#define MD_EXPONENT 65537u

/* 1024 bits in 32 bit limbs */
#define RSA_LIMBS 32

typedef uint32_t limb_t;

/* ------------------------------------------------------------ big integers */
/* Little-endian limb arrays of RSA_LIMBS limbs. Values are always kept below the modulus
 * where the operations below are used, which is what lets the reductions be a single
 * conditional subtraction. */

/* a -= b. A borrow out of the top is discarded: every caller here subtracts a value it
 * knows is not greater than a, or relies on the wrap-around of the shorter result. */
static void bn_sub(limb_t* a, const limb_t* b)
{
    uint32_t borrow = 0;
    for (int i = 0; i < RSA_LIMBS; ++i) {
        uint64_t difference = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (limb_t)difference;
        borrow = (uint32_t)(difference >> 32) & 1u;
    }
}

/* -1, 0 or 1 for a < b, a == b, a > b */
static int bn_cmp(const limb_t* a, const limb_t* b)
{
    for (int i = RSA_LIMBS - 1; i >= 0; --i) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

/* r = (a + b) mod m, with a and b below m. b may be a or m may not alias r. */
static void bn_addmod(limb_t* r, const limb_t* a, const limb_t* b, const limb_t* m)
{
    uint64_t carry = 0;
    for (int i = 0; i < RSA_LIMBS; ++i) {
        uint64_t sum = (uint64_t)a[i] + b[i] + carry;
        r[i] = (limb_t)sum;
        carry = sum >> 32;
    }
    /* Either the sum overflowed 1024 bits, in which case it is above the modulus, or it did
     * not and the comparison decides. In the overflow case the subtraction wraps, and the
     * wrapped result is exactly (a + b) - m. */
    if (carry != 0 || bn_cmp(r, m) >= 0) {
        bn_sub(r, m);
    }
}

/* r = 2a mod m, with a below m. r may be a. */
static void bn_dblmod(limb_t* r, const limb_t* a, const limb_t* m)
{
    uint32_t carry = 0;
    for (int i = 0; i < RSA_LIMBS; ++i) {
        limb_t value = a[i];
        r[i] = (limb_t)((value << 1) | carry);
        carry = value >> 31;
    }
    if (carry != 0 || bn_cmp(r, m) >= 0) {
        bn_sub(r, m);
    }
}

/* r = a * b mod m, with a and b below m. r may alias a or b. */
static void bn_mulmod(limb_t* r, const limb_t* a, const limb_t* b, const limb_t* m)
{
    limb_t accumulator[RSA_LIMBS];
    limb_t doubled[RSA_LIMBS];

    memset(accumulator, 0, sizeof(accumulator));
    memcpy(doubled, a, sizeof(doubled));
    for (int bit = 0; bit < RSA_LIMBS * 32; ++bit) {
        if (((b[bit / 32] >> (bit % 32)) & 1u) != 0) {
            bn_addmod(accumulator, accumulator, doubled, m);
        }
        bn_dblmod(doubled, doubled, m);
    }
    memcpy(r, accumulator, sizeof(accumulator));
}

/* r = base ^ exponent mod m, square and multiply over the exponent bits. */
static void bn_modexp(limb_t* r, const limb_t* base, uint32_t exponent, const limb_t* m)
{
    limb_t accumulator[RSA_LIMBS];
    limb_t power[RSA_LIMBS];

    memset(accumulator, 0, sizeof(accumulator));
    accumulator[0] = 1;
    memcpy(power, base, sizeof(power));
    while (exponent != 0) {
        if ((exponent & 1u) != 0) {
            bn_mulmod(accumulator, accumulator, power, m);
        }
        exponent >>= 1;
        if (exponent != 0) {
            bn_mulmod(power, power, power, m);
        }
    }
    memcpy(r, accumulator, sizeof(accumulator));
}

/* MD_AUTHEN_SIGNATURE_SIZE big-endian bytes into limbs. That is exactly RSA_LIMBS limbs,
 * so the input fills the array and cannot be longer. */
static void bn_from_bytes(limb_t* r, const uint8_t* in)
{
    memset(r, 0, RSA_LIMBS * sizeof(limb_t));
    for (int i = 0; i < MD_AUTHEN_SIGNATURE_SIZE; ++i) {
        int remaining = MD_AUTHEN_SIGNATURE_SIZE - 1 - i;
        r[remaining / 4] |= (limb_t)in[i] << (8 * (remaining % 4));
    }
}

static void bn_to_bytes(uint8_t* out, const limb_t* a)
{
    for (int i = 0; i < RSA_LIMBS; ++i) {
        for (int byte = 0; byte < 4; ++byte) {
            out[(RSA_LIMBS - 1 - i) * 4 + (3 - byte)] = (uint8_t)(a[i] >> (8 * byte));
        }
    }
}

/* --------------------------------------------------------------- encoding */

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/* Standard Base64, as android.util.Base64.NO_WRAP writes it: padded, with no line breaks.
 * Returns the number of bytes written or -1. */
static int base64_decode(const char* text, uint8_t* out, size_t out_cap)
{
    size_t length = strlen(text);
    if (length == 0 || length % 4 != 0) {
        return -1;
    }

    size_t written = 0;
    for (size_t group = 0; group < length; group += 4) {
        int value[4];
        int padding = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text[group + i];
            if (c == '=') {
                /* Padding only closes the last group, and only replaces its last one or two
                 * characters. */
                if (group + 4 < length || i < 2) {
                    return -1;
                }
                value[i] = 0;
                ++padding;
            } else {
                if (padding != 0) {
                    return -1;
                }
                value[i] = base64_value(c);
                if (value[i] < 0) {
                    return -1;
                }
            }
        }

        uint32_t bits = ((uint32_t)value[0] << 18) | ((uint32_t)value[1] << 12) |
                        ((uint32_t)value[2] << 6) | (uint32_t)value[3];
        int bytes = 3 - padding;
        for (int i = 0; i < bytes; ++i) {
            if (written >= out_cap) {
                return -1;
            }
            out[written++] = (uint8_t)(bits >> (16 - 8 * i));
        }
    }
    return (int)written;
}

/* Strip the PKCS#1 v1.5 block (00 01 FF ... FF 00) that the phone's RSA operation leaves
 * in front of the message. Returns 0 with *message and *message_len set, or -1. */
static int pkcs1_strip(const uint8_t* block, int block_len, const uint8_t** message, int* message_len)
{
    if (block_len < 11 || block[0] != 0x00 || block[1] != 0x01) {
        return -1;
    }
    int i = 2;
    while (i < block_len && block[i] == 0xff) {
        ++i;
    }
    /* The standard requires at least eight padding bytes. */
    if (i < 10 || i >= block_len || block[i] != 0x00) {
        return -1;
    }
    *message = block + i + 1;
    *message_len = block_len - i - 1;
    return 0;
}

/* ---------------------------------------------------------------- public */

int md_authen_recover(const char* encrypt_value, char* out, size_t out_cap)
{
    if (encrypt_value == NULL || out == NULL || out_cap == 0) {
        return -1;
    }

    uint8_t signature[MD_AUTHEN_SIGNATURE_SIZE];
    if (base64_decode(encrypt_value, signature, sizeof(signature)) != MD_AUTHEN_SIGNATURE_SIZE) {
        return -1;
    }

    limb_t modulus[RSA_LIMBS];
    limb_t block[RSA_LIMBS];
    limb_t plain[RSA_LIMBS];
    bn_from_bytes(modulus, MD_MODULUS);
    bn_from_bytes(block, signature);
    /* A block at or above the modulus has no meaningful decoding. */
    if (bn_cmp(block, modulus) >= 0) {
        return -1;
    }
    bn_modexp(plain, block, MD_EXPONENT, modulus);

    uint8_t block_bytes[MD_AUTHEN_SIGNATURE_SIZE];
    bn_to_bytes(block_bytes, plain);

    const uint8_t* message = NULL;
    int message_len = 0;
    if (pkcs1_strip(block_bytes, (int)sizeof(block_bytes), &message, &message_len) != 0) {
        return -1;
    }
    if ((size_t)message_len + 1 > out_cap) {
        return -1;
    }
    memcpy(out, message, (size_t)message_len);
    out[message_len] = '\0';
    return message_len;
}
