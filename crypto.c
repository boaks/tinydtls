/*******************************************************************************
 *
 * Copyright (c) 2011, 2012, 2013, 2014, 2015 Olaf Bergmann (TZI) and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v. 1.0 which accompanies this distribution.
 *
 * The Eclipse Public License is available at http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at 
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Olaf Bergmann  - initial API and implementation
 *    Hauke Mehrtens - memory optimization, ECC integration
 *
 *******************************************************************************/

#include <stdio.h>

#include "tinydtls.h"

#ifdef HAVE_ASSERT_H
#include <assert.h>
#else
#define assert(x)
#endif
#include <inttypes.h>

#include "global.h"
#include "dtls_debug.h"
#include "numeric.h"
#include "dtls.h"
#include "crypto.h"
#include "ccm.h"
#ifdef DTLS_ECC
#include "ext/micro-ecc/uECC.h"
#endif /* DTLS_ECC */
#include "dtls_prng.h"
#include "netq.h"

#include "dtls_mutex.h"

#ifdef WITH_ZEPHYR
LOG_MODULE_DECLARE(TINYDTLS, CONFIG_TINYDTLS_LOG_LEVEL);
#endif /* WITH_ZEPHYR */

#if defined(RIOT_VERSION)
# include <memarray.h>

dtls_handshake_parameters_t handshake_storage_data[DTLS_HANDSHAKE_MAX];
dtls_security_parameters_t security_storage_data[DTLS_SECURITY_MAX];
dtls_handshake_parameters_t handshake_storage_data[DTLS_HANDSHAKE_MAX];
dtls_security_parameters_t security_storage_data[DTLS_SECURITY_MAX];

memarray_t handshake_storage;
memarray_t security_storage;
memarray_t handshake_storage;
memarray_t security_storage;

#endif /* RIOT_VERSION */

#define HMAC_UPDATE_SEED(Context,Seed,Length)		\
  if (Seed) dtls_hmac_update(Context, (Seed), (Length))

static struct dtls_cipher_context_t cipher_context;
static dtls_mutex_t cipher_context_mutex = DTLS_MUTEX_INITIALIZER;

static struct dtls_cipher_context_t *dtls_cipher_context_get(void)
{
  dtls_mutex_lock(&cipher_context_mutex);
  return &cipher_context;
}

static void dtls_cipher_context_release(void)
{
  dtls_mutex_unlock(&cipher_context_mutex);
}

#if !(defined (WITH_CONTIKI)) && !(defined (RIOT_VERSION))
void crypto_init(void)
{
}

static dtls_handshake_parameters_t *dtls_handshake_malloc(void) {
  return malloc(sizeof(dtls_handshake_parameters_t));
}

static void dtls_handshake_dealloc(dtls_handshake_parameters_t *handshake) {
  free(handshake);
}

static dtls_security_parameters_t *dtls_security_malloc(void) {
  return malloc(sizeof(dtls_security_parameters_t));
}

static void dtls_security_dealloc(dtls_security_parameters_t *security) {
  free(security);
}
#elif defined (WITH_CONTIKI) /* WITH_CONTIKI */

#include "memb.h"
MEMB(handshake_storage, dtls_handshake_parameters_t, DTLS_HANDSHAKE_MAX);
MEMB(security_storage, dtls_security_parameters_t, DTLS_SECURITY_MAX);

void crypto_init(void) {
  memb_init(&handshake_storage);
  memb_init(&security_storage);
}

static dtls_handshake_parameters_t *dtls_handshake_malloc(void) {
  return memb_alloc(&handshake_storage);
}

static void dtls_handshake_dealloc(dtls_handshake_parameters_t *handshake) {
  memb_free(&handshake_storage, handshake);
}

static dtls_security_parameters_t *dtls_security_malloc(void) {
  return memb_alloc(&security_storage);
}

static void dtls_security_dealloc(dtls_security_parameters_t *security) {
  memb_free(&security_storage, security);
}

#elif defined (RIOT_VERSION)

void crypto_init(void) {
  memarray_init(&handshake_storage, handshake_storage_data, sizeof(dtls_handshake_parameters_t), DTLS_HANDSHAKE_MAX);
  memarray_init(&security_storage, security_storage_data, sizeof(dtls_security_parameters_t), DTLS_SECURITY_MAX);
}

static dtls_handshake_parameters_t *dtls_handshake_malloc(void) {
  return memarray_alloc(&handshake_storage);
}

static void dtls_security_dealloc(dtls_security_parameters_t *security) {
  memarray_free(&security_storage, security);
}

static dtls_security_parameters_t *dtls_security_malloc(void) {
  return memarray_alloc(&security_storage);
}

static void dtls_handshake_dealloc(dtls_handshake_parameters_t *handshake) {
  memarray_free(&handshake_storage, handshake);
}

#endif /* WITH_CONTIKI */

dtls_handshake_parameters_t *dtls_handshake_new(void)
{
  dtls_handshake_parameters_t *handshake;

  handshake = dtls_handshake_malloc();
  if (!handshake) {
    dtls_crit("can not allocate a handshake struct\n");
    return NULL;
  }

  memset(handshake, 0, sizeof(*handshake));

  /* initialize the handshake hash wrt. the hard-coded DTLS version */
  dtls_debug("DTLSv12: initialize HASH_SHA256\n");
  /* TLS 1.2:  PRF(secret, label, seed) = P_<hash>(secret, label + seed) */
  /* FIXME: we use the default SHA256 here, might need to support other
            hash functions as well */
  dtls_hash_init(&handshake->hs_state.hs_hash);
  return handshake;
}

void dtls_handshake_free(dtls_handshake_parameters_t *handshake)
{
  if (!handshake)
    return;

  netq_delete_all(&handshake->reorder_queue);
  dtls_handshake_dealloc(handshake);
}

dtls_security_parameters_t *dtls_security_new(void)
{
  dtls_security_parameters_t *security;

  security = dtls_security_malloc();
  if (!security) {
    dtls_crit("can not allocate a security struct\n");
    return NULL;
  }

  memset(security, 0, sizeof(*security));

  security->cipher_index = DTLS_CIPHER_INDEX_NULL;
  security->compression = TLS_COMPRESSION_NULL;

  return security;
}

void dtls_security_free(dtls_security_parameters_t *security)
{
  if (!security)
    return;

  dtls_security_dealloc(security);
}

size_t
dtls_p_hash(dtls_hashfunc_t h,
	    const unsigned char *key, size_t keylen,
	    const unsigned char *label, size_t labellen,
	    const unsigned char *random1, size_t random1len,
	    const unsigned char *random2, size_t random2len,
	    unsigned char *buf, size_t buflen) {
  dtls_hmac_context_t hmac;

  unsigned char A[DTLS_HMAC_DIGEST_SIZE];
  unsigned char tmp[DTLS_HMAC_DIGEST_SIZE];
  size_t dlen;			/* digest length */
  size_t len = 0;			/* result length */
  (void)h;

  dtls_hmac_init(&hmac, key, keylen);

  /* calculate A(1) from A(0) == seed */
  HMAC_UPDATE_SEED(&hmac, label, labellen);
  HMAC_UPDATE_SEED(&hmac, random1, random1len);
  HMAC_UPDATE_SEED(&hmac, random2, random2len);

  dlen = dtls_hmac_finalize(&hmac, A);

  while (len < buflen) {
    dtls_hmac_init(&hmac, key, keylen);
    dtls_hmac_update(&hmac, A, dlen);

    HMAC_UPDATE_SEED(&hmac, label, labellen);
    HMAC_UPDATE_SEED(&hmac, random1, random1len);
    HMAC_UPDATE_SEED(&hmac, random2, random2len);

    dlen = dtls_hmac_finalize(&hmac, tmp);

    if ((len + dlen) < buflen) {
        memcpy(&buf[len], tmp, dlen);
        len += dlen;
    }
    else {
        memcpy(&buf[len], tmp, buflen - len);
        break;
    }

    /* calculate A(i+1) */
    dtls_hmac_init(&hmac, key, keylen);
    dtls_hmac_update(&hmac, A, dlen);
    dtls_hmac_finalize(&hmac, A);
  }

  /* prevent exposure of sensible data */
  memset(&hmac, 0, sizeof(hmac));
  memset(tmp, 0, sizeof(tmp));
  memset(A, 0, sizeof(A));

  return buflen;
}

size_t 
dtls_prf(const unsigned char *key, size_t keylen,
	 const unsigned char *label, size_t labellen,
	 const unsigned char *random1, size_t random1len,
	 const unsigned char *random2, size_t random2len,
	 unsigned char *buf, size_t buflen) {

  /* Clear the result buffer */
  memset(buf, 0, buflen);
  return dtls_p_hash(HASH_SHA256, 
		     key, keylen, 
		     label, labellen, 
		     random1, random1len,
		     random2, random2len,
		     buf, buflen);
}

void
dtls_mac(dtls_hmac_context_t *hmac_ctx, 
	 const unsigned char *record,
	 const unsigned char *packet, size_t length,
	 unsigned char *buf) {
  uint16 L;
  dtls_int_to_uint16(L, length);

  assert(hmac_ctx);
  dtls_hmac_update(hmac_ctx, record +3, sizeof(uint16) + sizeof(uint48));
  dtls_hmac_update(hmac_ctx, record, sizeof(uint8) + sizeof(uint16));
  dtls_hmac_update(hmac_ctx, L, sizeof(uint16));
  dtls_hmac_update(hmac_ctx, packet, length);
  
  dtls_hmac_finalize(hmac_ctx, buf);
}

static size_t
dtls_ccm_encrypt(aes128_ccm_t *ccm_ctx, const unsigned char *src, size_t srclen,
		 unsigned char *buf, 
		 const unsigned char *nonce,
		 const unsigned char *aad, size_t la) {
  long int len;
  (void)src;

  assert(ccm_ctx);

  len = dtls_ccm_encrypt_message(&ccm_ctx->ctx,
                                 ccm_ctx->tag_length /* M */,
				 ccm_ctx->l /* L */,
				 nonce,
				 buf, srclen,
				 aad, la);
  return len;
}

static size_t
dtls_ccm_decrypt(aes128_ccm_t *ccm_ctx, const unsigned char *src,
		 size_t srclen, unsigned char *buf,
		 const unsigned char *nonce,
		 const unsigned char *aad, size_t la) {
  long int len;
  (void)src;

  assert(ccm_ctx);

  len = dtls_ccm_decrypt_message(&ccm_ctx->ctx,
                                 ccm_ctx->tag_length /* M */,
				 ccm_ctx->l /* L */,
				 nonce,
				 buf, srclen,
				 aad, la);
  return len;
}

#ifdef DTLS_PSK
int
dtls_psk_pre_master_secret(unsigned char *key, size_t keylen,
			   unsigned char *result, size_t result_len) {
  unsigned char *p = result;

  if (result_len < (2 * (sizeof(uint16) + keylen))) {
    return -1;
  }

  dtls_int_to_uint16(p, keylen);
  p += sizeof(uint16);

  memset(p, 0, keylen);
  p += keylen;

  memcpy(p, result, sizeof(uint16));
  p += sizeof(uint16);
  
  memcpy(p, key, keylen);

  return 2 * (sizeof(uint16) + keylen);
}
#endif /* DTLS_PSK */

#ifdef DTLS_ECC
#ifdef uECC_SUPPORTS_secp256r1
const dtls_ecdh_curve default_curve = TLS_EXT_ELLIPTIC_CURVES_SECP256R1;
#endif /* uECC_SUPPORTS_secp256r1 */

static void dtls_ec_key_to_uint32(const unsigned char *key, size_t key_size,
				  uint32_t *result) {
  int i;

  for (i = (key_size / sizeof(uint32_t)) - 1; i >= 0 ; i--) {
    *result = dtls_uint32_to_int(&key[i * sizeof(uint32_t)]);
    result++;
  }
}

static void dtls_ec_key_from_uint32(const uint32_t *key, size_t key_size,
				    unsigned char *result) {
  int i;

  for (i = (key_size / sizeof(uint32_t)) - 1; i >= 0 ; i--) {
    dtls_int_to_uint32(result, key[i]);
    result += 4;
  }
}

/* Build the EC KEY as a ASN.1 positive integer */
/*
 * The public EC key consists of two positive numbers. Converting them into
 * ASN.1 INTEGER requires removing leading zeros, but special care must be
 * taken of the resulting sign. If the first non-zero byte of the 32 byte
 * ec-key has bit 7 set (highest bit), the resultant ASN.1 INTEGER would be
 * interpreted as a negative number. In order to prevent this, a zero in the
 * ASN.1 presentation is prepended if that bit 7 is set.
*/
int dtls_ec_key_asn1_from_uint32(const uint32_t *key, size_t key_size,
				 uint8_t *buf) {
  int i = 0;
  uint8_t *lptr;
   
  /* ASN.1 Integer r */
  dtls_int_to_uint8(buf, 0x02);
  buf += sizeof(uint8);

  lptr = buf;
  /* Length will be filled in later */
  buf += sizeof(uint8);

  dtls_ec_key_from_uint32(key, key_size, buf);
  
  /* skip leading 0's */
  while (i < (int)key_size && buf[i] == 0) {
     ++i;
  }
  assert(i != (int)key_size);
  if (i == (int)key_size) {
      dtls_alert("ec key is all zero\n");
      return 0;
  }
  if (buf[i] >= 0x80) {
    /* 
     * Preserve unsigned by adding leading 0 (i may go negative which is
     * explicitely handled below with the assumption that buf is at least 33
     * bytes in size).
     */
     --i;
  }
  if (i > 0) {
      /* remove leading 0's */
      key_size -= i;
      memmove(buf, buf + i, key_size);
  } else if (i == -1) {
      /* add leading 0 */
      memmove(buf +1, buf, key_size);
      buf[0] = 0;
      key_size++;
  }
  /* Update the length of positive ASN.1 integer */
  dtls_int_to_uint8(lptr, key_size);
  return key_size + 2; 
}

static int get_uecc_curve(dtls_ecdh_curve curve, uECC_Curve *result) {
  struct {
    dtls_ecdh_curve curve;
    uECC_Curve uecc_curve;
  } known_curves[] = {
#if uECC_SUPPORTS_secp256r1
    { TLS_EXT_ELLIPTIC_CURVES_SECP256R1, uECC_secp256r1() },
#endif /* uECC_SUPPORTS_secp256r1 */
  };
  unsigned int index;

  for (index = 0; index < sizeof(known_curves)/sizeof(known_curves[0]); index++) {
    if (known_curves[index].curve == curve) {
      *result = known_curves[index].uecc_curve;
      return 1;
    }
  }
  return 0;
}

int dtls_ecdh_pre_master_secret2(const unsigned char *priv_key,
                                 const unsigned char *pub_key,
                                 size_t key_size,
                                 dtls_ecdh_curve curve,
                                 unsigned char *result,
                                 size_t result_len) {
  uECC_Curve uecc_curve;
  if (!get_uecc_curve(curve, &uecc_curve)) {
    dtls_warn("curve %" PRIu16 " not supported\n", curve);
    return -1;
  }

  if (result_len < key_size) {
    return -1;
  }

  if (!uECC_valid_public_key(pub_key, uecc_curve)) {
    dtls_warn("invalid public key\n");
  }

  if (!uECC_shared_secret(pub_key, priv_key, result, uecc_curve)) {
    dtls_warn("cannot generate ECDH shared secret\n");
    return 0;
  }

  return key_size;
}

int dtls_ecdh_pre_master_secret(unsigned char *priv_key,
				   unsigned char *pub_key_x,
                                   unsigned char *pub_key_y,
                                   size_t key_size,
                                   unsigned char *result,
                                   size_t result_len) {
  const dtls_ecdh_curve curve = default_curve;
  uint8_t pub_key[2 * DTLS_EC_KEY_SIZE];

  if (result_len < key_size) {
    return -1;
  }

  memcpy(pub_key, pub_key_x, DTLS_EC_KEY_SIZE);
  memcpy(pub_key + DTLS_EC_KEY_SIZE, pub_key_y, DTLS_EC_KEY_SIZE);
  return dtls_ecdh_pre_master_secret2(priv_key, pub_key, key_size, curve,
                                      result, result_len);
}

void
dtls_ecdsa_generate_key(unsigned char *priv_key,
			unsigned char *pub_key_x,
			unsigned char *pub_key_y,
			size_t key_size) {
  const dtls_ecdh_curve curve = default_curve;
  uint8_t pub_key[2 * DTLS_EC_KEY_SIZE];

  int res = dtls_ecdsa_generate_key2(priv_key, pub_key, key_size, curve);
  if (res > 0) {
    memcpy(pub_key_x, pub_key, res);
    memcpy(pub_key_y, pub_key + res, res);
  }
}

int
dtls_ecdsa_generate_key2(unsigned char *priv_key,
                         unsigned char *pub_key,
                         size_t key_size,
                         dtls_ecdh_curve curve) {
  uECC_Curve uecc_curve;
  if (!get_uecc_curve(curve, &uecc_curve)) {
    dtls_warn("curve %" PRIu16 " not supported\n", curve);
    return -1;
  }

  assert(key_size >= (unsigned int)uECC_curve_private_key_size(uecc_curve));
  assert(2 * key_size >= (unsigned int)uECC_curve_public_key_size(uecc_curve));

  if (!uECC_make_key(pub_key, priv_key, uecc_curve)
      || !uECC_valid_public_key(pub_key, uecc_curve)) {
    dtls_crit("cannot generate ECC key pair\n");
    return 0;
  }
  return uECC_curve_private_key_size(uecc_curve);
}

/* rfc4492#section-5.4 */
void
dtls_ecdsa_create_sig_hash(const unsigned char *priv_key, size_t key_size,
			   const unsigned char *sign_hash, size_t sign_hash_size,
			   uint32_t point_r[9], uint32_t point_s[9]) {
  const dtls_ecdh_curve curve = default_curve;
  dtls_ecdsa_create_sig_hash2(priv_key, key_size,
                              sign_hash, sign_hash_size,
                              curve, point_r, point_s);

}

int
dtls_ecdsa_create_sig_hash2(const unsigned char *priv_key, size_t key_size,
                            const unsigned char *sign_hash, size_t sign_hash_size,
                            dtls_ecdh_curve curve,
                            uint32_t point_r[9], uint32_t point_s[9]) {
  uint8_t sign[2 * DTLS_EC_KEY_SIZE];
  uECC_Curve uecc_curve;
  int curve_size;
  if (!get_uecc_curve(curve, &uecc_curve)) {
    dtls_warn("curve %" PRIu16 " not supported\n", curve);
    return -1;
  }

  curve_size = uECC_curve_private_key_size(uecc_curve);

  assert(key_size >= (unsigned int)curve_size);
  assert(sign_hash_size >= (unsigned int)curve_size);
  assert(sizeof(sign) >= 2 * (unsigned int)curve_size);
  if (!uECC_sign(priv_key, sign_hash, sign_hash_size, sign, uecc_curve)) {
    dtls_warn("cannot create signature\n");
    return -1;
  }

  dtls_ec_key_to_uint32(sign, curve_size, point_r);
  dtls_ec_key_to_uint32(sign + curve_size, curve_size, point_s);
  return 2 * curve_size;
}

void
dtls_ecdsa_create_sig(const unsigned char *priv_key, size_t key_size,
		      const unsigned char *client_random, size_t client_random_size,
		      const unsigned char *server_random, size_t server_random_size,
		      const unsigned char *keyx_params, size_t keyx_params_size,
		      uint32_t point_r[9], uint32_t point_s[9]) {
  dtls_hash_ctx data;
  unsigned char sha256hash[DTLS_HMAC_DIGEST_SIZE];

  dtls_hash_init(&data);
  dtls_hash_update(&data, client_random, client_random_size);
  dtls_hash_update(&data, server_random, server_random_size);
  dtls_hash_update(&data, keyx_params, keyx_params_size);
  dtls_hash_finalize(sha256hash, &data);
  
  dtls_ecdsa_create_sig_hash(priv_key, key_size, sha256hash,
			     sizeof(sha256hash), point_r, point_s);
}

/* rfc4492#section-5.4 */
int
dtls_ecdsa_verify_sig_hash(const unsigned char *pub_key_x,
			   const unsigned char *pub_key_y, size_t key_size,
			   const unsigned char *sign_hash, size_t sign_hash_size,
			   unsigned char *result_r, unsigned char *result_s) {
  const dtls_ecdh_curve curve = default_curve;
  uint8_t pub_key[2 * DTLS_EC_KEY_SIZE];
  assert(sizeof(pub_key) >= (key_size * 2));
  memcpy(pub_key, pub_key_x, key_size);
  memcpy(pub_key + key_size, pub_key_y, key_size);
  return dtls_ecdsa_verify_sig_hash2(pub_key, key_size * 2,
                                     sign_hash, sign_hash_size,
                                     curve,
                                     result_r, result_s);
}

int
dtls_ecdsa_verify_sig_hash2(const unsigned char *pub_key, size_t key_size,
                            const unsigned char *sign_hash, size_t sign_hash_size,
                            dtls_ecdh_curve curve,
                            unsigned char *result_r, unsigned char *result_s) {
  uint8_t sign[2 * DTLS_EC_KEY_SIZE];
  uECC_Curve uecc_curve;
  int curve_size;
  if (!get_uecc_curve(curve, &uecc_curve)) {
    dtls_warn("curve %" PRIu16 " not supported\n", curve);
    return -1;
  }
  (void)result_r;
  (void)result_s;

  curve_size = uECC_curve_public_key_size(uecc_curve);
  assert(key_size == (unsigned int)curve_size);
  assert(sizeof(sign) >= (unsigned int)curve_size);

  /* clear sign to avoid maybe-unitialized warning */
  memset(sign, 0, sizeof(sign));
  return uECC_verify(pub_key, sign_hash, sign_hash_size, sign, uecc_curve);
}

int
dtls_ecdsa_verify_sig(const unsigned char *pub_key_x,
		      const unsigned char *pub_key_y, size_t key_size,
		      const unsigned char *client_random, size_t client_random_size,
		      const unsigned char *server_random, size_t server_random_size,
		      const unsigned char *keyx_params, size_t keyx_params_size,
		      unsigned char *result_r, unsigned char *result_s) {
  dtls_hash_ctx data;
  unsigned char sha256hash[DTLS_HMAC_DIGEST_SIZE];
  
  dtls_hash_init(&data);
  dtls_hash_update(&data, client_random, client_random_size);
  dtls_hash_update(&data, server_random, server_random_size);
  dtls_hash_update(&data, keyx_params, keyx_params_size);
  dtls_hash_finalize(sha256hash, &data);

  return dtls_ecdsa_verify_sig_hash(pub_key_x, pub_key_y, key_size, sha256hash,
				    sizeof(sha256hash), result_r, result_s);
}
#endif /* DTLS_ECC */

int
dtls_encrypt_params(const dtls_ccm_params_t *params,
                    const unsigned char *src, size_t length,
                    unsigned char *buf,
                    const unsigned char *key, size_t keylen,
                    const unsigned char *aad, size_t la) {
  int ret;
  struct dtls_cipher_context_t *ctx = dtls_cipher_context_get();
  ctx->data.tag_length = params->tag_length;
  ctx->data.l = params->l;

  ret = rijndael_set_key_enc_only(&ctx->data.ctx, key, 8 * keylen);
  if (ret < 0) {
    /* cleanup everything in case the key has the wrong size */
    dtls_warn("cannot set rijndael key\n");
    goto error;
  }

  if (src != buf)
    memmove(buf, src, length);
  ret = dtls_ccm_encrypt(&ctx->data, src, length, buf, params->nonce, aad, la);

error:
  dtls_cipher_context_release();
  return ret;
}

int 
dtls_encrypt(const unsigned char *src, size_t length,
	     unsigned char *buf,
	     const unsigned char *nonce,
	     const unsigned char *key, size_t keylen,
	     const unsigned char *aad, size_t la)
{
  /* For backwards-compatibility, dtls_encrypt_params is called with
   * M=8 and L=3. */
  const dtls_ccm_params_t params = { nonce, 8, 3 };

  return dtls_encrypt_params(&params, src, length, buf, key, keylen, aad, la);
}

int
dtls_decrypt_params(const dtls_ccm_params_t *params,
                    const unsigned char *src, size_t length,
                    unsigned char *buf,
                    const unsigned char *key, size_t keylen,
                    const unsigned char *aad, size_t la)
{
  int ret;
  struct dtls_cipher_context_t *ctx = dtls_cipher_context_get();
  ctx->data.tag_length = params->tag_length;
  ctx->data.l = params->l;

  ret = rijndael_set_key_enc_only(&ctx->data.ctx, key, 8 * keylen);
  if (ret < 0) {
    /* cleanup everything in case the key has the wrong size */
    dtls_warn("cannot set rijndael key\n");
    goto error;
  }

  if (src != buf)
    memmove(buf, src, length);
  ret = dtls_ccm_decrypt(&ctx->data, src, length, buf, params->nonce, aad, la);

error:
  dtls_cipher_context_release();
  return ret;
}

int
dtls_decrypt(const unsigned char *src, size_t length,
	     unsigned char *buf,
	     const unsigned char *nonce,
	     const unsigned char *key, size_t keylen,
	     const unsigned char *aad, size_t la)
{
  /* For backwards-compatibility, dtls_encrypt_params is called with
   * M=8 and L=3. */
  const dtls_ccm_params_t params = { nonce, 8, 3 };

  return dtls_decrypt_params(&params, src, length, buf, key, keylen, aad, la);
}
