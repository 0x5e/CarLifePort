/*
 * Authentication of the CarLife phone app -- the head unit side of the exchange in
 * section 17.1 of the protocol guide.
 *
 * The head unit asks the phone to prove it is the official app: it sends a random value in
 * MSG_CMD_HU_AUTHEN_REQUEST (0x00018048) and the phone answers in
 * MSG_CMD_MD_AUTHEN_RESPONSE (0x00010049) with that value encrypted under the RSA private
 * key built into its own code. Checking the answer means running the matching public key
 * operation and comparing the recovered text with the value that was sent.
 *
 * The public key is not part of the head unit library; it is the public half of
 * com.baidu.carlife.authorization.PrivateKeyManager.PRIVATE_KEY, taken from the phone app
 * (reverse engineering report CARLIFE_AUTH.md section 1.1, see README "Head unit
 * authentication"). Only the public operation is implemented here, so nothing secret is
 * involved on this side.
 */
#ifndef CARLIFE_LINK_MD_AUTHEN_H
#define CARLIFE_LINK_MD_AUTHEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The phone's answer is a single RSA-1024 block, Base64 encoded when it travels in the
 * protocol (callers base64-decode nothing themselves: md_authen_recover takes the value as
 * it arrives). */
#define MD_AUTHEN_SIGNATURE_SIZE 128

/* Longest message a PKCS#1 v1.5 block of that size can carry (128 - 11 bytes of header and
 * padding). A larger buffer is never needed and a response that would need one is not a
 * valid answer. */
#define MD_AUTHEN_MAX_MESSAGE 117

/*
 * Recover the text the phone signed. <encrypt_value> is the Base64 value from
 * MSG_CMD_MD_AUTHEN_RESPONSE. The result is written to <out> as a NUL-terminated string of
 * at most <out_cap> bytes.
 *
 * Returns the length of the recovered text, or -1 when the value is not a well-formed
 * response: bad Base64, not one full block, at least the modulus, or a block whose
 * PKCS#1 padding does not parse. A returned text is whatever the private key actually
 * signed -- comparing it with the value that was sent is the caller's job, because the
 * phone rewrites the version part of that value when the version it is asked for is
 * newer than its own (see the report, section 1.3).
 */
int md_authen_recover(const char* encrypt_value, char* out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* CARLIFE_LINK_MD_AUTHEN_H */
