/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/crypto/crypto.h>
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

	zassert_equal(pkt.out_len, sizeof(expected));
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
