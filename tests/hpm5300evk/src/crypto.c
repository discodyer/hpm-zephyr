/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/crypto/crypto.h>
#include <drivers/crypto/hpmicro_sdp.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#define SDP_NODE DT_NODELABEL(sdp)

static const uint8_t aes128_key[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

static const uint8_t aes256_key[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
	0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static const uint8_t ecb_plaintext[] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

static const uint8_t ecb128_ciphertext[] = {
	0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
	0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
};

static const uint8_t ecb256_ciphertext[] = {
	0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
	0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89,
};

static const uint8_t cbc_key[] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
};

static const uint8_t cbc_iv[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

static const uint8_t cbc_plaintext[] = {
	0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
	0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
};

static const uint8_t cbc_ciphertext[] = {
	0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
	0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
};

static const uint8_t ctr_iv[] = {
	0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb,
};

static const uint8_t ctr_plaintext[] = {
	0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73,
	0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7,
	0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4,
	0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45,
	0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10,
};

static const uint8_t ctr_ciphertext[] = {
	0x22, 0xe5, 0x2f, 0xb1, 0x77, 0xd8, 0x65, 0xb2, 0xf7, 0xc6, 0xb5, 0x12, 0x69,
	0x2d, 0x11, 0x4d, 0xed, 0x6c, 0x1c, 0x72, 0x25, 0xda, 0xf6, 0xa2, 0xaa, 0xd9,
	0xd3, 0xda, 0x2d, 0xba, 0x21, 0x68, 0x35, 0xc0, 0xaf, 0x6b, 0x6f, 0x40, 0xc3,
	0xc6, 0xef, 0xc5, 0x85, 0xd0, 0x90, 0x2c, 0xc2, 0x63, 0x12, 0x2b, 0xc5, 0x8e,
	0x72, 0xde, 0x5c, 0xa2, 0xa3, 0x5c, 0x85, 0x3a, 0xb9, 0x2c, 0x06, 0xbb,
};

static const uint8_t sha256_abc[] = {
	0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
	0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
	0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

static int aes_ecb_run(const uint8_t *key, size_t key_len, const uint8_t *expected)
{
	const struct device *dev = DEVICE_DT_GET(SDP_NODE);
	uint8_t input[sizeof(ecb_plaintext)];
	uint8_t output[sizeof(ecb_plaintext)];
	struct cipher_ctx ctx = {
		.keylen = key_len,
		.key.bit_stream = key,
		.flags = CAP_RAW_KEY | CAP_SYNC_OPS | CAP_SEPARATE_IO_BUFS,
	};
	struct cipher_pkt pkt = {
		.in_buf = input,
		.in_len = sizeof(input),
		.out_buf = output,
		.out_buf_max = sizeof(output),
	};
	int ret;

	memcpy(input, ecb_plaintext, sizeof(input));
	ret = cipher_begin_session(dev, &ctx, CRYPTO_CIPHER_ALGO_AES, CRYPTO_CIPHER_MODE_ECB,
				   CRYPTO_CIPHER_OP_ENCRYPT);
	if (ret != 0) {
		return ret;
	}

	ret = cipher_block_op(&ctx, &pkt);
	(void)cipher_free_session(dev, &ctx);
	if (ret != 0) {
		return ret;
	}

	zassert_equal(pkt.out_len, sizeof(ecb_plaintext));
	return memcmp(output, expected, sizeof(ecb_plaintext));
}

static int aes_cbc_run(void)
{
	const struct device *dev = DEVICE_DT_GET(SDP_NODE);
	uint8_t iv[sizeof(cbc_iv)];
	uint8_t ciphertext[sizeof(cbc_iv) + sizeof(cbc_ciphertext)];
	uint8_t plaintext[sizeof(cbc_plaintext)];
	struct cipher_ctx ctx = {
		.keylen = sizeof(cbc_key),
		.key.bit_stream = cbc_key,
		.flags = CAP_RAW_KEY | CAP_SYNC_OPS | CAP_SEPARATE_IO_BUFS,
	};
	struct cipher_pkt pkt = {
		.in_buf = (uint8_t *)cbc_plaintext,
		.in_len = sizeof(cbc_plaintext),
		.out_buf = ciphertext,
		.out_buf_max = sizeof(ciphertext),
	};
	int ret;

	memcpy(iv, cbc_iv, sizeof(iv));
	ret = cipher_begin_session(dev, &ctx, CRYPTO_CIPHER_ALGO_AES, CRYPTO_CIPHER_MODE_CBC,
				   CRYPTO_CIPHER_OP_ENCRYPT);
	if (ret != 0) {
		return ret;
	}

	ret = cipher_cbc_op(&ctx, &pkt, iv);
	(void)cipher_free_session(dev, &ctx);
	if (ret != 0) {
		return ret;
	}

	zassert_equal(pkt.out_len, sizeof(ciphertext));
	zassert_mem_equal(ciphertext, cbc_iv, sizeof(cbc_iv));
	zassert_mem_equal(ciphertext + sizeof(cbc_iv), cbc_ciphertext, sizeof(cbc_ciphertext));

	ctx.keylen = sizeof(cbc_key);
	ctx.key.bit_stream = cbc_key;
	ctx.flags = CAP_RAW_KEY | CAP_SYNC_OPS | CAP_SEPARATE_IO_BUFS;
	pkt.in_buf = ciphertext;
	pkt.in_len = sizeof(ciphertext);
	pkt.out_buf = plaintext;
	pkt.out_buf_max = sizeof(plaintext);

	memcpy(iv, cbc_iv, sizeof(iv));
	ret = cipher_begin_session(dev, &ctx, CRYPTO_CIPHER_ALGO_AES, CRYPTO_CIPHER_MODE_CBC,
				   CRYPTO_CIPHER_OP_DECRYPT);
	if (ret != 0) {
		return ret;
	}

	ret = cipher_cbc_op(&ctx, &pkt, iv);
	(void)cipher_free_session(dev, &ctx);
	if (ret != 0) {
		return ret;
	}

	zassert_equal(pkt.out_len, sizeof(plaintext));
	return memcmp(plaintext, cbc_plaintext, sizeof(plaintext));
}

static int aes_ctr_run(void)
{
	const struct device *dev = DEVICE_DT_GET(SDP_NODE);
	uint8_t iv[sizeof(ctr_iv)];
	uint8_t output[sizeof(ctr_plaintext)];
	struct cipher_ctx ctx = {
		.keylen = sizeof(cbc_key),
		.key.bit_stream = cbc_key,
		.flags = CAP_RAW_KEY | CAP_SYNC_OPS | CAP_SEPARATE_IO_BUFS,
		.mode_params.ctr_info.ctr_len = 32U,
	};
	struct cipher_pkt pkt = {
		.in_buf = (uint8_t *)ctr_plaintext,
		.in_len = sizeof(ctr_plaintext),
		.out_buf = output,
		.out_buf_max = sizeof(output),
	};
	int ret;

	memcpy(iv, ctr_iv, sizeof(iv));
	ret = cipher_begin_session(dev, &ctx, CRYPTO_CIPHER_ALGO_AES, CRYPTO_CIPHER_MODE_CTR,
				   CRYPTO_CIPHER_OP_ENCRYPT);
	if (ret != 0) {
		return ret;
	}

	ret = cipher_ctr_op(&ctx, &pkt, iv);
	(void)cipher_free_session(dev, &ctx);
	if (ret != 0) {
		return ret;
	}

	zassert_equal(pkt.out_len, sizeof(output));
	return memcmp(output, ctr_ciphertext, sizeof(output));
}

static int aes_ccm_run(void)
{
	const struct device *dev = DEVICE_DT_GET(SDP_NODE);
	static const uint8_t key[] = {
		0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
		0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
	};
	static const uint8_t nonce[] = {0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00,
					0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5};
	static const uint8_t aad[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
	static const uint8_t plaintext[] = {
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
		0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,
	};
	static const uint8_t ciphertext[] = {
		0x58, 0x8c, 0x97, 0x9a, 0x61, 0xc6, 0x63, 0xd2, 0xf0, 0x66, 0xd0,
		0xc2, 0xc0, 0xf9, 0x89, 0x80, 0x6d, 0x5f, 0x6b, 0x61, 0xda, 0xc3,
		0x84, 0x17, 0xe8, 0xd1, 0x2c, 0xfd, 0xf9, 0x26, 0xe0,
	};
	uint8_t output[sizeof(plaintext)];
	uint8_t tag[8];
	uint8_t decrypted[sizeof(plaintext)];
	uint8_t nonce_copy[sizeof(nonce)];
	struct cipher_ctx ctx = {
		.keylen = sizeof(key),
		.key.bit_stream = key,
		.mode_params.ccm_info =
			{
				.nonce_len = sizeof(nonce),
				.tag_len = sizeof(tag),
			},
		.flags = CAP_RAW_KEY | CAP_SYNC_OPS | CAP_SEPARATE_IO_BUFS,
	};
	struct cipher_pkt pkt = {
		.in_buf = (uint8_t *)plaintext,
		.in_len = sizeof(plaintext),
		.out_buf = output,
		.out_buf_max = sizeof(output),
	};
	struct cipher_aead_pkt aead = {
		.pkt = &pkt,
		.ad = (uint8_t *)aad,
		.ad_len = sizeof(aad),
		.tag = tag,
	};
	int ret;

	memcpy(nonce_copy, nonce, sizeof(nonce_copy));
	ret = cipher_begin_session(dev, &ctx, CRYPTO_CIPHER_ALGO_AES, CRYPTO_CIPHER_MODE_CCM,
				   CRYPTO_CIPHER_OP_ENCRYPT);
	if (ret != 0) {
		return ret;
	}

	ret = cipher_ccm_op(&ctx, &aead, nonce_copy);
	(void)cipher_free_session(dev, &ctx);
	if (ret != 0) {
		return ret;
	}

	zassert_mem_equal(output, ciphertext, sizeof(output));
	zassert_mem_equal(tag, ciphertext + sizeof(output), sizeof(tag));

	ctx.key.bit_stream = key;
	ctx.flags = CAP_RAW_KEY | CAP_SYNC_OPS | CAP_SEPARATE_IO_BUFS;
	pkt.in_buf = output;
	pkt.out_buf = decrypted;
	pkt.out_buf_max = sizeof(decrypted);
	aead.tag = tag;
	memcpy(nonce_copy, nonce, sizeof(nonce_copy));
	ret = cipher_begin_session(dev, &ctx, CRYPTO_CIPHER_ALGO_AES, CRYPTO_CIPHER_MODE_CCM,
				   CRYPTO_CIPHER_OP_DECRYPT);
	if (ret != 0) {
		return ret;
	}

	ret = cipher_ccm_op(&ctx, &aead, nonce_copy);
	(void)cipher_free_session(dev, &ctx);
	if (ret != 0) {
		return ret;
	}

	return memcmp(decrypted, plaintext, sizeof(decrypted));
}

static int hpm_sdp_hashes_run(void)
{
	const struct device *dev = DEVICE_DT_GET(SDP_NODE);
	static const uint8_t message[] = "abc";
	static const uint8_t sha1_abc[] = {
		0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
		0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d,
	};
	static const uint8_t crc32_abc[] = {0xff, 0x7c, 0x12, 0x7d};
	static const uint8_t sm3_abc[] = {
		0x66, 0xc7, 0xf0, 0xf4, 0x62, 0xee, 0xed, 0xd9, 0xd1, 0xf2, 0xd4,
		0x6b, 0xdc, 0x10, 0xe4, 0xe2, 0x41, 0x67, 0xc4, 0x87, 0x5c, 0xf2,
		0xf7, 0xa2, 0x29, 0x7d, 0xa0, 0x2b, 0x8f, 0x4b, 0xa8, 0xe0,
	};
	uint8_t digest[32];

	zassert_ok(hpm_sdp_hash_compute(dev, HPM_SDP_HASH_SHA1, message, sizeof(message) - 1U,
					digest, sizeof(sha1_abc)));
	zassert_mem_equal(digest, sha1_abc, sizeof(sha1_abc));

	zassert_ok(hpm_sdp_hash_compute(dev, HPM_SDP_HASH_CRC32, message, sizeof(message) - 1U,
					digest, sizeof(crc32_abc)));
	zassert_mem_equal(digest, crc32_abc, sizeof(crc32_abc));

	zassert_ok(hpm_sdp_hash_compute(dev, HPM_SDP_HASH_SM3, message, sizeof(message) - 1U,
					digest, sizeof(sm3_abc)));
	zassert_mem_equal(digest, sm3_abc, sizeof(sm3_abc));

	zassert_equal(hpm_sdp_hash_compute(dev, HPM_SDP_HASH_SHA1, message, sizeof(message) - 1U,
					   digest, sizeof(digest)),
		      -EINVAL);
	zassert_equal(
		hpm_sdp_hash_compute(dev, HPM_SDP_HASH_SHA256, NULL, 0U, digest, sizeof(digest)),
		-EINVAL);

	return 0;
}

static int sm4_run(void)
{
	const struct device *dev = DEVICE_DT_GET(SDP_NODE);
	static const uint8_t key[] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
		0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
	};
	static const uint8_t plaintext[] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
		0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
	};
	static const uint8_t ecb_ciphertext[] = {
		0x68, 0x1e, 0xdf, 0x34, 0xd2, 0x06, 0x96, 0x5e,
		0x86, 0xb3, 0xe9, 0x4f, 0x53, 0x6e, 0x42, 0x46,
	};
	static const uint8_t cbc_iv[] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	};
	static const uint8_t cbc_ciphertext[] = {
		0x5d, 0x1f, 0xee, 0x63, 0xf5, 0xeb, 0x8b, 0xb5,
		0x03, 0x58, 0x0a, 0xb8, 0x23, 0x92, 0x5d, 0x55,
	};
	uint8_t output[sizeof(plaintext)];
	uint8_t decrypted[sizeof(plaintext)];

	zassert_ok(hpm_sdp_sm4_crypt(dev, HPM_SDP_CIPHER_ENCRYPT, HPM_SDP_SM4_ECB, key, NULL,
				     plaintext, sizeof(plaintext), output));
	zassert_mem_equal(output, ecb_ciphertext, sizeof(output));
	zassert_ok(hpm_sdp_sm4_crypt(dev, HPM_SDP_CIPHER_DECRYPT, HPM_SDP_SM4_ECB, key, NULL,
				     output, sizeof(output), decrypted));
	zassert_mem_equal(decrypted, plaintext, sizeof(decrypted));

	zassert_ok(hpm_sdp_sm4_crypt(dev, HPM_SDP_CIPHER_ENCRYPT, HPM_SDP_SM4_CBC, key, cbc_iv,
				     plaintext, sizeof(plaintext), output));
	zassert_mem_equal(output, cbc_ciphertext, sizeof(output));
	zassert_ok(hpm_sdp_sm4_crypt(dev, HPM_SDP_CIPHER_DECRYPT, HPM_SDP_SM4_CBC, key, cbc_iv,
				     output, sizeof(output), decrypted));
	zassert_mem_equal(decrypted, plaintext, sizeof(decrypted));

	zassert_equal(hpm_sdp_sm4_crypt(dev, HPM_SDP_CIPHER_ENCRYPT, HPM_SDP_SM4_CBC, key, NULL,
					plaintext, sizeof(plaintext), output),
		      -EINVAL);
	zassert_equal(hpm_sdp_sm4_crypt(dev, HPM_SDP_CIPHER_ENCRYPT, HPM_SDP_SM4_ECB, key, NULL,
					plaintext, sizeof(plaintext) - 1U, output),
		      -EINVAL);

	return 0;
}

static int sha256_run(void)
{
	const struct device *dev = DEVICE_DT_GET(SDP_NODE);
	static const uint8_t message[] = "abc";
	uint8_t digest[sizeof(sha256_abc)];
	struct hash_ctx ctx = {
		.flags = CAP_SYNC_OPS | CAP_SEPARATE_IO_BUFS,
	};
	struct hash_pkt pkt = {
		.in_buf = message,
		.in_len = sizeof(message) - 1U,
		.out_buf = digest,
	};
	int ret;

	ret = hash_begin_session(dev, &ctx, CRYPTO_HASH_ALGO_SHA256);
	if (ret != 0) {
		return ret;
	}

	/* Reusing the session exercises reinitialization between digests. */
	for (size_t i = 0U; i < 2U; i++) {
		memset(digest, 0, sizeof(digest));
		pkt.in_buf = message;
		pkt.in_len = sizeof(message) - 1U;
		pkt.out_buf = digest;
		ret = hash_compute(&ctx, &pkt);
		if (ret != 0) {
			break;
		}
		zassert_mem_equal(digest, sha256_abc, sizeof(digest));
	}

	/* Multipart updates must produce the same digest. */
	if (ret == 0) {
		pkt.in_buf = message;
		pkt.in_len = 1U;
		pkt.out_buf = NULL;
		ret = hash_update(&ctx, &pkt);
	}
	if (ret == 0) {
		pkt.in_buf = message + 1U;
		pkt.in_len = sizeof(message) - 2U;
		ret = hash_update(&ctx, &pkt);
	}
	if (ret == 0) {
		memset(digest, 0, sizeof(digest));
		pkt.in_buf = NULL;
		pkt.in_len = 0U;
		pkt.out_buf = digest;
		ret = hash_compute(&ctx, &pkt);
	}
	if (ret == 0) {
		zassert_mem_equal(digest, sha256_abc, sizeof(digest));
	}

	(void)hash_free_session(dev, &ctx);
	return ret;
}

ZTEST(hpm5300evk, test_sdp_crypto_ready)
{
	const struct device *dev = DEVICE_DT_GET(SDP_NODE);
	int caps;

	zassert_true(device_is_ready(dev));
	caps = crypto_query_hwcaps(dev);
	zassert_true((caps & CAP_RAW_KEY) != 0);
	zassert_true((caps & CAP_SEPARATE_IO_BUFS) != 0);
	zassert_true((caps & CAP_SYNC_OPS) != 0);
}

ZTEST(hpm5300evk, test_sdp_aes_kat)
{
	zassert_ok(aes_ecb_run(aes128_key, sizeof(aes128_key), ecb128_ciphertext));
	zassert_ok(aes_ecb_run(aes256_key, sizeof(aes256_key), ecb256_ciphertext));
	zassert_ok(aes_cbc_run());
	zassert_ok(aes_ctr_run());
}

ZTEST(hpm5300evk, test_sdp_sha256_kat)
{
	zassert_ok(sha256_run());
}

ZTEST(hpm5300evk, test_sdp_ccm_kat)
{
	zassert_ok(aes_ccm_run());
}

ZTEST(hpm5300evk, test_sdp_extended_hash_kat)
{
	zassert_ok(hpm_sdp_hashes_run());
}

ZTEST(hpm5300evk, test_sdp_sm4_kat)
{
	zassert_ok(sm4_run());
}
