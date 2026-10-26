/*
* JJY-SIM - Storage Crypto
*
* Provides lightweight encryption for application settings.
*
* - Uses AES-CTR for encrypting small data (e.g. SSID / password)
* - Generates random nonce per encryption
* - Designed to avoid plain text storage in NVS
*
* Note:
* This is not intended for strong security, but to prevent
* casual inspection of stored data.
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 *
 * Developed by Shachi-lab
 * https://shachi-lab.com
 *   ____  _                _     _     _       _
 *  / ___)| |__   __ _  ___| |__ |_|   | | __ _| |__
 *  \___ \| '_ \ / _` |/ __) '_ \ _  _ | |/ _` | '_ \
 *   ___) | | | | (_| | (__| | | | ||_|| | (_| | |_) |
 *  (____/|_| |_|\__,_|\___)_| |_|_|   |_|\__,_|_.__/
 */
#include <string.h>
#include "esp_random.h"
#include "mbedtls/aes.h"

#include "storage_crypto.h"
#include "resource/storage_key.h"

static const char *TAG __attribute__((unused)) = "storage_crypto";

static const uint8_t s_storage_crypto_key[16] = { STORAGE_CRYPTO_KEY };

static bool aes_ctr_crypt(
  const uint8_t key[16],
  const uint8_t nonce[STORAGE_CRYPTO_NONCE_LEN],
  const uint8_t *in,
  uint8_t *out,
  size_t len)
{
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);

  if (mbedtls_aes_setkey_enc(&aes, key, 128) != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }

  uint8_t stream_block[16] = {0};
  uint8_t nonce_counter[16];
  size_t nc_off = 0;

  memcpy(nonce_counter, nonce, 16);

  int ret = mbedtls_aes_crypt_ctr(
    &aes,
    len,
    &nc_off,
    nonce_counter,
    stream_block,
    in,
    out
);

  mbedtls_aes_free(&aes);
  return ret == 0;
}

bool storage_encrypt(const char *plain, storage_crypto_blob_t *out)
{
  if (!plain || !out) {
    return false;
  }

  size_t len = strlen(plain);
  if (len == 0 || len > STORAGE_CRYPTO_MAX_PLAIN) {
    return false;
  }

  memset(out, 0, sizeof(*out));
  out->len = (uint8_t)len;

  // nonce は毎回ランダム
  esp_fill_random(out->nonce, STORAGE_CRYPTO_NONCE_LEN);

  return aes_ctr_crypt(
    s_storage_crypto_key,
    out->nonce,
    (const uint8_t *)plain,
    out->data,
    len
  );
}

bool storage_decrypt(const storage_crypto_blob_t *in, char *out, size_t out_size)
{
  if (!in || !out) {
    return false;
  }

  if (in->len == 0 || in->len > STORAGE_CRYPTO_MAX_PLAIN) {
    return false;
  }

  if (out_size < ((size_t)in->len + 1)) {
    return false;
  }

  memset(out, 0, out_size);

  if (!aes_ctr_crypt(
      s_storage_crypto_key,
      in->nonce,
      in->data,
      (uint8_t *)out,
      in->len)) {
    return false;
  }

  out[in->len] = '\0';
  return true;
}