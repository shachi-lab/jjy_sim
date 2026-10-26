/*
 * JJY-SIM - Storage Crypto (API)
 *
 * Provides simple encryption/decryption for small data.
 *
 * Intended for protecting stored settings from casual inspection.
 * Not designed for strong security.
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#ifndef _STORAGE_CRYPTO_H_
#define _STORAGE_CRYPTO_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define STORAGE_CRYPTO_NONCE_LEN    16
#define STORAGE_CRYPTO_MAX_PLAIN    64

typedef struct {
  uint8_t nonce[STORAGE_CRYPTO_NONCE_LEN];
  uint8_t len;                   // Plain length
  uint8_t data[STORAGE_CRYPTO_MAX_PLAIN];
} storage_crypto_blob_t;

// Encrypt string into storage blob
bool storage_encrypt(const char *plain, storage_crypto_blob_t *out);

// Decrypt storage blob into string
bool storage_decrypt(const storage_crypto_blob_t *in, char *out, size_t out_size);

#endif
