/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_CRYPTO_HPMICRO_SDP_H_
#define ZEPHYR_INCLUDE_DRIVERS_CRYPTO_HPMICRO_SDP_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro secure data processor extensions
 * @defgroup hpm_sdp_interface HPMicro SDP
 * @ingroup crypto_interface
 * @{
 */

/** Hash algorithms without a matching Zephyr hash_algo value. */
enum hpm_sdp_hash_algorithm {
	/** SHA-1, 20-byte digest. */
	HPM_SDP_HASH_SHA1 = 0,
	/** CRC-32, four-byte little-endian result. */
	HPM_SDP_HASH_CRC32,
	/** SHA-256, 32-byte digest. */
	HPM_SDP_HASH_SHA256,
	/** SM3, 32-byte digest. */
	HPM_SDP_HASH_SM3,
};

/** Cipher operation. */
enum hpm_sdp_cipher_op {
	HPM_SDP_CIPHER_ENCRYPT = 0,
	HPM_SDP_CIPHER_DECRYPT,
};

/** SM4 cipher mode. */
enum hpm_sdp_sm4_mode {
	HPM_SDP_SM4_ECB = 0,
	HPM_SDP_SM4_CBC,
	HPM_SDP_SM4_CTR,
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_sdp_driver_api {
	/** Must remain first so the device also implements the crypto API. */
	struct crypto_driver_api crypto_api;
	int (*hash_compute)(const struct device *dev, enum hpm_sdp_hash_algorithm algorithm,
			    const uint8_t *input, size_t input_len, uint8_t *digest,
			    size_t digest_len);
	int (*sm4_crypt)(const struct device *dev, enum hpm_sdp_cipher_op op,
			 enum hpm_sdp_sm4_mode mode, const uint8_t *key, const uint8_t *iv,
			 const uint8_t *input, size_t input_len, uint8_t *output);
};

DEVICE_API_EXTENDS(hpm_sdp, crypto, crypto_api);
/** @endcond */

/**
 * @brief Compute a digest using an SDP-only hash algorithm.
 *
 * This API exposes SHA-1, CRC-32, SHA-256 and SM3 without inventing values
 * in Zephyr's standard hash_algo enumeration. The call is synchronous.
 *
 * @param dev SDP device.
 * @param algorithm Hash algorithm.
 * @param input Input bytes.
 * @param input_len Number of input bytes.
 * @param digest Output digest.
 * @param digest_len Size of @p digest; it must equal the algorithm digest size.
 *
 * @retval 0 on success.
 * @retval -EINVAL for null pointers or an incorrect digest length.
 * @retval -ENOTSUP if the selected algorithm is unavailable in silicon.
 */
static inline int hpm_sdp_hash_compute(const struct device *dev,
				       enum hpm_sdp_hash_algorithm algorithm, const uint8_t *input,
				       size_t input_len, uint8_t *digest, size_t digest_len)
{
	return DEVICE_API_GET(hpm_sdp, dev)
		->hash_compute(dev, algorithm, input, input_len, digest, digest_len);
}

/**
 * @brief Perform a synchronous SM4 operation.
 *
 * ECB ignores @p iv. CBC requires a 16-byte IV. CTR requires a full 16-byte
 * nonce/counter block in the byte order expected by the SDP hardware. The
 * input and output buffers must not overlap.
 *
 * @param dev SDP device.
 * @param op Encrypt or decrypt.
 * @param mode ECB, CBC or CTR.
 * @param key 16-byte SM4 key.
 * @param iv IV or nonce/counter block, depending on @p mode.
 * @param input Input bytes.
 * @param input_len Number of input bytes.
 * @param output Output buffer, at least @p input_len bytes.
 *
 * @retval 0 on success.
 * @retval -EINVAL for invalid parameters or ECB/CBC length.
 * @retval -ENOTSUP if SM4 is unavailable or input exceeds the bounce limit.
 */
static inline int hpm_sdp_sm4_crypt(const struct device *dev, enum hpm_sdp_cipher_op op,
				    enum hpm_sdp_sm4_mode mode, const uint8_t *key,
				    const uint8_t *iv, const uint8_t *input, size_t input_len,
				    uint8_t *output)
{
	return DEVICE_API_GET(hpm_sdp, dev)
		->sm4_crypt(dev, op, mode, key, iv, input, input_len, output);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_CRYPTO_HPMICRO_SDP_H_ */
