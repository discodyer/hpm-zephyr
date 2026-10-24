/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_sdp

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/crypto/crypto.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hpm_sdp_drv.h>

LOG_MODULE_REGISTER(crypto_hpmicro, CONFIG_CRYPTO_LOG_LEVEL);

#define HPM_CRYPTO_AES_BLOCK_SIZE  16U
#define HPM_CRYPTO_AES_KEY_LEN_128 16U
#define HPM_CRYPTO_AES_KEY_LEN_256 32U
#define HPM_CRYPTO_SHA256_LEN      32U
#define HPM_CRYPTO_AES_CHUNK_SIZE                                                                  \
	ROUND_DOWN(CONFIG_CRYPTO_HPMICRO_BOUNCE_SIZE, HPM_CRYPTO_AES_BLOCK_SIZE)

#define HPM_CRYPTO_CIPHER_CAPS                                                                     \
	(CAP_RAW_KEY | CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS | CAP_NO_IV_PREFIX)
#define HPM_CRYPTO_HASH_CAPS (CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS)

struct hpm_crypto_config {
	SDP_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct hpm_crypto_session {
	bool in_use;
	uint8_t key[HPM_CRYPTO_AES_KEY_LEN_256];
	uint16_t key_len;
	enum cipher_op cipher_op;
	bool hash_started;
	sdp_aes_ctx_t aes_ctx;
	sdp_hash_ctx_t hash_ctx;
};

struct hpm_crypto_data {
	struct k_mutex session_lock;
	struct k_mutex hw_lock;
	struct hpm_crypto_session sessions[CONFIG_CRYPTO_HPMICRO_MAX_SESSIONS];
	uint8_t ctr_block[HPM_CRYPTO_AES_BLOCK_SIZE] __aligned(HPM_L1C_CACHELINE_SIZE);
	uint8_t bounce_in[CONFIG_CRYPTO_HPMICRO_BOUNCE_SIZE] __aligned(HPM_L1C_CACHELINE_SIZE);
	uint8_t bounce_out[CONFIG_CRYPTO_HPMICRO_BOUNCE_SIZE] __aligned(HPM_L1C_CACHELINE_SIZE);
};

BUILD_ASSERT(HPM_CRYPTO_AES_CHUNK_SIZE >= HPM_CRYPTO_AES_BLOCK_SIZE,
	     "SDP bounce buffer is too small for one AES block");

#ifdef CONFIG_DCACHE_LINE_SIZE
BUILD_ASSERT(HPM_L1C_CACHELINE_SIZE == CONFIG_DCACHE_LINE_SIZE,
	     "SDP and Zephyr cache-line sizes differ");
#endif

static int hpm_crypto_status_to_errno(hpm_stat_t status)
{
	switch (status) {
	case status_success:
		return 0;
	case status_invalid_argument:
	case status_sdp_invalid_key_src:
	case status_sdp_invalid_alg:
		return -EINVAL;
	case status_sdp_no_crypto_support:
	case status_sdp_no_hash_support:
		return -ENOTSUP;
	case status_sdp_aes_busy:
	case status_sdp_hash_busy:
		return -EBUSY;
	default:
		return -EIO;
	}
}

static struct hpm_crypto_session *hpm_crypto_get_session(const struct device *dev)
{
	struct hpm_crypto_data *data = dev->data;
	struct hpm_crypto_session *session = NULL;

	k_mutex_lock(&data->session_lock, K_FOREVER);
	for (size_t i = 0U; i < ARRAY_SIZE(data->sessions); i++) {
		if (!data->sessions[i].in_use) {
			session = &data->sessions[i];
			memset(session, 0, sizeof(*session));
			session->in_use = true;
			break;
		}
	}
	k_mutex_unlock(&data->session_lock);

	return session;
}

static void hpm_crypto_free_session(struct hpm_crypto_data *data,
				    struct hpm_crypto_session *session)
{
	k_mutex_lock(&data->session_lock, K_FOREVER);
	memset(session, 0, sizeof(*session));
	compiler_barrier();
	k_mutex_unlock(&data->session_lock);
}

static int hpm_crypto_query_hw_caps(const struct device *dev)
{
	ARG_UNUSED(dev);

	return HPM_CRYPTO_CIPHER_CAPS | HPM_CRYPTO_HASH_CAPS;
}

static int hpm_crypto_aes_ecb(struct cipher_ctx *ctx, struct cipher_pkt *pkt)
{
	const struct hpm_crypto_config *config = ctx->device->config;
	struct hpm_crypto_data *data = ctx->device->data;
	struct hpm_crypto_session *session = ctx->drv_sessn_state;
	sdp_aes_key_bits_t key_bits = session->key_len == HPM_CRYPTO_AES_KEY_LEN_128
					      ? sdp_aes_keybits_128
					      : sdp_aes_keybits_256;
	sdp_aes_op_t op = session->cipher_op == CRYPTO_CIPHER_OP_ENCRYPT ? sdp_aes_op_encrypt
									 : sdp_aes_op_decrypt;
	hpm_stat_t status;
	int ret;

	if ((pkt == NULL) || (pkt->in_buf == NULL) || (pkt->out_buf == NULL)) {
		return -EINVAL;
	}

	if ((pkt->in_len != HPM_CRYPTO_AES_BLOCK_SIZE) || (pkt->out_buf_max < 0) ||
	    (pkt->out_buf_max < HPM_CRYPTO_AES_BLOCK_SIZE)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->hw_lock, K_FOREVER);

	status = sdp_aes_set_key(config->base, &session->aes_ctx, session->key, key_bits, 0U);
	if (status != status_success) {
		k_mutex_unlock(&data->hw_lock);
		return hpm_crypto_status_to_errno(status);
	}

	memcpy(data->bounce_in, pkt->in_buf, HPM_CRYPTO_AES_BLOCK_SIZE);
	status = sdp_aes_crypt_ecb(config->base, &session->aes_ctx, op, HPM_CRYPTO_AES_BLOCK_SIZE,
				   data->bounce_in, data->bounce_out);
	if (status == status_success) {
		memcpy(pkt->out_buf, data->bounce_out, HPM_CRYPTO_AES_BLOCK_SIZE);
		pkt->out_len = HPM_CRYPTO_AES_BLOCK_SIZE;
	}

	k_mutex_unlock(&data->hw_lock);
	ret = hpm_crypto_status_to_errno(status);

	return ret;
}

static int hpm_crypto_aes_cbc(struct cipher_ctx *ctx, struct cipher_pkt *pkt, uint8_t *iv)
{
	const struct hpm_crypto_config *config = ctx->device->config;
	struct hpm_crypto_data *data = ctx->device->data;
	struct hpm_crypto_session *session = ctx->drv_sessn_state;
	sdp_aes_key_bits_t key_bits = session->key_len == HPM_CRYPTO_AES_KEY_LEN_128
					      ? sdp_aes_keybits_128
					      : sdp_aes_keybits_256;
	sdp_aes_op_t op = session->cipher_op == CRYPTO_CIPHER_OP_ENCRYPT ? sdp_aes_op_encrypt
									 : sdp_aes_op_decrypt;
	uint8_t cur_iv[HPM_CRYPTO_AES_BLOCK_SIZE];
	uint8_t next_iv[HPM_CRYPTO_AES_BLOCK_SIZE];
	const uint8_t *input;
	uint8_t *output;
	size_t data_len;
	size_t offset = 0U;
	bool prefix_iv;
	hpm_stat_t status;
	int ret = 0;

	if ((pkt == NULL) || (iv == NULL) || (pkt->in_len < 0) || (pkt->out_buf_max < 0)) {
		return -EINVAL;
	}

	prefix_iv = (ctx->flags & CAP_NO_IV_PREFIX) == 0U;
	if ((op == sdp_aes_op_encrypt) && prefix_iv) {
		/* Zephyr's default CBC contract prefixes the output with the IV. */
		if ((pkt->in_len % HPM_CRYPTO_AES_BLOCK_SIZE) != 0) {
			return -EINVAL;
		}
		input = pkt->in_buf;
		data_len = pkt->in_len;
	} else if (op == sdp_aes_op_decrypt) {
		if (prefix_iv) {
			if ((pkt->in_buf == NULL) || (pkt->in_len < HPM_CRYPTO_AES_BLOCK_SIZE) ||
			    ((pkt->in_len - HPM_CRYPTO_AES_BLOCK_SIZE) %
				     HPM_CRYPTO_AES_BLOCK_SIZE !=
			     0)) {
				return -EINVAL;
			}
			input = pkt->in_buf + HPM_CRYPTO_AES_BLOCK_SIZE;
			data_len = pkt->in_len - HPM_CRYPTO_AES_BLOCK_SIZE;
		} else {
			if ((pkt->in_len % HPM_CRYPTO_AES_BLOCK_SIZE) != 0) {
				return -EINVAL;
			}
			input = pkt->in_buf;
			data_len = pkt->in_len;
		}
	} else {
		if ((pkt->in_len % HPM_CRYPTO_AES_BLOCK_SIZE) != 0) {
			return -EINVAL;
		}
		input = pkt->in_buf;
		data_len = pkt->in_len;
	}

	memcpy(cur_iv, iv, HPM_CRYPTO_AES_BLOCK_SIZE);
	if (((data_len != 0U) && (input == NULL)) ||
	    ((pkt->out_buf == NULL) &&
	     (data_len != 0U || (prefix_iv && (op == sdp_aes_op_encrypt))))) {
		return -EINVAL;
	}

	if ((size_t)pkt->out_buf_max <
	    data_len + (prefix_iv && (op == sdp_aes_op_encrypt) ? HPM_CRYPTO_AES_BLOCK_SIZE : 0U)) {
		return -EINVAL;
	}

	output = (prefix_iv && (op == sdp_aes_op_encrypt))
			 ? pkt->out_buf + HPM_CRYPTO_AES_BLOCK_SIZE
			 : pkt->out_buf;

	k_mutex_lock(&data->hw_lock, K_FOREVER);

	status = sdp_aes_set_key(config->base, &session->aes_ctx, session->key, key_bits, 0U);
	if (status != status_success) {
		ret = hpm_crypto_status_to_errno(status);
		goto unlock;
	}

	while (offset < data_len) {
		size_t chunk = MIN(data_len - offset, (size_t)HPM_CRYPTO_AES_CHUNK_SIZE);

		memcpy(data->bounce_in, input + offset, chunk);
		if ((op == sdp_aes_op_decrypt) && (offset + chunk < data_len)) {
			memcpy(next_iv, input + offset + chunk - HPM_CRYPTO_AES_BLOCK_SIZE,
			       HPM_CRYPTO_AES_BLOCK_SIZE);
		}

		status = sdp_aes_crypt_cbc(config->base, &session->aes_ctx, op, chunk, cur_iv,
					   data->bounce_in, data->bounce_out);
		if (status != status_success) {
			ret = hpm_crypto_status_to_errno(status);
			goto unlock;
		}

		memcpy(output + offset, data->bounce_out, chunk);
		offset += chunk;
		if (offset < data_len) {
			if (op == sdp_aes_op_encrypt) {
				memcpy(next_iv,
				       data->bounce_out + chunk - HPM_CRYPTO_AES_BLOCK_SIZE,
				       HPM_CRYPTO_AES_BLOCK_SIZE);
			}
			memcpy(cur_iv, next_iv, HPM_CRYPTO_AES_BLOCK_SIZE);
		}
	}

	if (prefix_iv && (op == sdp_aes_op_encrypt)) {
		memcpy(pkt->out_buf, iv, HPM_CRYPTO_AES_BLOCK_SIZE);
	}

	pkt->out_len = data_len +
		       (prefix_iv && (op == sdp_aes_op_encrypt) ? HPM_CRYPTO_AES_BLOCK_SIZE : 0U);

unlock:
	k_mutex_unlock(&data->hw_lock);

	return ret;
}

static int hpm_crypto_aes_ctr(struct cipher_ctx *ctx, struct cipher_pkt *pkt, uint8_t *iv)
{
	const struct hpm_crypto_config *config = ctx->device->config;
	struct hpm_crypto_data *data = ctx->device->data;
	struct hpm_crypto_session *session = ctx->drv_sessn_state;
	sdp_aes_key_bits_t key_bits = session->key_len == HPM_CRYPTO_AES_KEY_LEN_128
					      ? sdp_aes_keybits_128
					      : sdp_aes_keybits_256;
	uint32_t ctr_len_bits = ctx->mode_params.ctr_info.ctr_len;
	size_t ctr_len;
	size_t iv_len;
	size_t offset = 0U;
	hpm_stat_t status;
	int ret;

	if ((pkt == NULL) || (pkt->in_len < 0) || (pkt->out_buf_max < 0)) {
		return -EINVAL;
	}

	if ((ctr_len_bits < 32U) ||
	    (ctr_len_bits > ((HPM_CRYPTO_AES_BLOCK_SIZE - 1U) * BITS_PER_BYTE)) ||
	    ((ctr_len_bits % BITS_PER_BYTE) != 0U)) {
		return -EINVAL;
	}

	ctr_len = ctr_len_bits / BITS_PER_BYTE;
	iv_len = HPM_CRYPTO_AES_BLOCK_SIZE - ctr_len;
	if ((iv_len != 0U) && (iv == NULL)) {
		return -EINVAL;
	}

	if ((pkt->in_len != 0) && ((pkt->in_buf == NULL) || (pkt->out_buf == NULL))) {
		return -EINVAL;
	}

	if (pkt->out_buf_max < pkt->in_len) {
		return -EINVAL;
	}

	k_mutex_lock(&data->hw_lock, K_FOREVER);

	memset(data->ctr_block, 0, sizeof(data->ctr_block));
	if (iv_len != 0U) {
		memcpy(data->ctr_block, iv, iv_len);
	}
	/* Zephyr's split counter starts at zero for the first block. */

	status = sdp_aes_set_key(config->base, &session->aes_ctx, session->key, key_bits, 0U);
	if (status != status_success) {
		ret = hpm_crypto_status_to_errno(status);
		goto unlock;
	}

	while (offset < (size_t)pkt->in_len) {
		size_t chunk = MIN((size_t)pkt->in_len - offset, (size_t)HPM_CRYPTO_AES_CHUNK_SIZE);

		memcpy(data->bounce_in, pkt->in_buf + offset, chunk);
		status = sdp_aes_crypt_ctr(config->base, &session->aes_ctx, data->ctr_block,
					   data->bounce_in, data->bounce_out, chunk);
		if (status != status_success) {
			ret = hpm_crypto_status_to_errno(status);
			goto unlock;
		}
		memcpy(pkt->out_buf + offset, data->bounce_out, chunk);
		offset += chunk;
	}

	pkt->out_len = pkt->in_len;
	ret = 0;

unlock:
	k_mutex_unlock(&data->hw_lock);

	return ret;
}

static int hpm_crypto_cipher_begin_session(const struct device *dev, struct cipher_ctx *ctx,
					   enum cipher_algo algo, enum cipher_mode mode,
					   enum cipher_op op_type)
{
	struct hpm_crypto_session *session;

	if ((ctx == NULL) || (algo != CRYPTO_CIPHER_ALGO_AES)) {
		return -ENOTSUP;
	}

	if ((ctx->flags & ~HPM_CRYPTO_CIPHER_CAPS) != 0U) {
		return -ENOTSUP;
	}

	if ((ctx->key.bit_stream == NULL) || ((ctx->keylen != HPM_CRYPTO_AES_KEY_LEN_128) &&
					      (ctx->keylen != HPM_CRYPTO_AES_KEY_LEN_256))) {
		return -EINVAL;
	}

	if ((op_type != CRYPTO_CIPHER_OP_ENCRYPT) && (op_type != CRYPTO_CIPHER_OP_DECRYPT)) {
		return -EINVAL;
	}

	if ((mode != CRYPTO_CIPHER_MODE_ECB) && (mode != CRYPTO_CIPHER_MODE_CBC) &&
	    (mode != CRYPTO_CIPHER_MODE_CTR)) {
		return -ENOTSUP;
	}

	if (mode == CRYPTO_CIPHER_MODE_CTR) {
		uint32_t ctr_len_bits = ctx->mode_params.ctr_info.ctr_len;

		if ((ctr_len_bits < 32U) ||
		    (ctr_len_bits > ((HPM_CRYPTO_AES_BLOCK_SIZE - 1U) * BITS_PER_BYTE)) ||
		    ((ctr_len_bits % BITS_PER_BYTE) != 0U)) {
			return -EINVAL;
		}
	}

	session = hpm_crypto_get_session(dev);
	if (session == NULL) {
		return -ENOSPC;
	}

	session->key_len = ctx->keylen;
	memcpy(session->key, ctx->key.bit_stream, ctx->keylen);
	session->cipher_op = op_type;

	switch (mode) {
	case CRYPTO_CIPHER_MODE_ECB:
		ctx->ops.block_crypt_hndlr = hpm_crypto_aes_ecb;
		break;
	case CRYPTO_CIPHER_MODE_CBC:
		ctx->ops.cbc_crypt_hndlr = hpm_crypto_aes_cbc;
		break;
	case CRYPTO_CIPHER_MODE_CTR:
		ctx->ops.ctr_crypt_hndlr = hpm_crypto_aes_ctr;
		break;
	default:
		hpm_crypto_free_session(dev->data, session);
		return -ENOTSUP;
	}

	ctx->ops.cipher_mode = mode;
	ctx->drv_sessn_state = session;

	return 0;
}

static int hpm_crypto_cipher_free_session(const struct device *dev, struct cipher_ctx *ctx)
{
	struct hpm_crypto_session *session;

	ARG_UNUSED(dev);

	if ((ctx == NULL) || (ctx->drv_sessn_state == NULL)) {
		return -EINVAL;
	}

	session = ctx->drv_sessn_state;
	hpm_crypto_free_session(dev->data, session);
	ctx->drv_sessn_state = NULL;
	memset(&ctx->ops, 0, sizeof(ctx->ops));

	return 0;
}

static int hpm_crypto_sha256(struct hash_ctx *ctx, struct hash_pkt *pkt, bool finish)
{
	const struct hpm_crypto_config *config = ctx->device->config;
	struct hpm_crypto_data *data = ctx->device->data;
	struct hpm_crypto_session *session = ctx->drv_sessn_state;
	uint8_t *digest;
	size_t offset = 0U;
	hpm_stat_t status = status_success;
	int ret = 0;

	if ((pkt == NULL) || ((pkt->in_len != 0U) && (pkt->in_buf == NULL))) {
		return -EINVAL;
	}

	k_mutex_lock(&data->hw_lock, K_FOREVER);

	if (!session->hash_started) {
		status = sdp_hash_init(config->base, &session->hash_ctx, sdp_hash_alg_sha256);
		if (status != status_success) {
			ret = hpm_crypto_status_to_errno(status);
			goto unlock;
		}
	}

	while (offset < pkt->in_len) {
		size_t chunk = MIN(pkt->in_len - offset, (size_t)CONFIG_CRYPTO_HPMICRO_BOUNCE_SIZE);

		memcpy(data->bounce_in, pkt->in_buf + offset, chunk);
		status = sdp_hash_update(config->base, &session->hash_ctx, data->bounce_in, chunk);
		if (status != status_success) {
			ret = hpm_crypto_status_to_errno(status);
			(void)sdp_hash_init(config->base, &session->hash_ctx, sdp_hash_alg_sha256);
			session->hash_started = false;
			ctx->started = false;
			goto unlock;
		}
		offset += chunk;
	}

	if (!finish) {
		session->hash_started = true;
		ctx->started = true;
		goto unlock;
	}

	digest = pkt->out_buf;
	if (digest == NULL) {
		if ((pkt->in_buf == NULL) || (pkt->in_len < HPM_CRYPTO_SHA256_LEN)) {
			ret = -EINVAL;
			session->hash_started = false;
			ctx->started = false;
			goto unlock;
		}
		digest = (uint8_t *)pkt->in_buf;
	}

	status = sdp_hash_finish(config->base, &session->hash_ctx, digest);
	ret = hpm_crypto_status_to_errno(status);

	session->hash_started = false;
	ctx->started = false;
	(void)sdp_hash_init(config->base, &session->hash_ctx, sdp_hash_alg_sha256);

unlock:
	k_mutex_unlock(&data->hw_lock);

	return ret;
}

static int hpm_crypto_hash_begin_session(const struct device *dev, struct hash_ctx *ctx,
					 enum hash_algo algo)
{
	const struct hpm_crypto_config *config = dev->config;
	struct hpm_crypto_data *data = dev->data;
	struct hpm_crypto_session *session;
	hpm_stat_t status;
	int ret;

	if (algo != CRYPTO_HASH_ALGO_SHA256) {
		return -ENOTSUP;
	}

	if ((ctx == NULL) || ((ctx->flags & ~HPM_CRYPTO_HASH_CAPS) != 0U)) {
		return -ENOTSUP;
	}

	session = hpm_crypto_get_session(dev);
	if (session == NULL) {
		return -ENOSPC;
	}

	k_mutex_lock(&data->hw_lock, K_FOREVER);
	status = sdp_hash_init(config->base, &session->hash_ctx, sdp_hash_alg_sha256);
	k_mutex_unlock(&data->hw_lock);
	ret = hpm_crypto_status_to_errno(status);
	if (ret != 0) {
		hpm_crypto_free_session(dev->data, session);
		return ret;
	}

	session->hash_started = false;
	ctx->hash_hndlr = hpm_crypto_sha256;
	ctx->started = false;
	ctx->drv_sessn_state = session;

	return 0;
}

static int hpm_crypto_hash_free_session(const struct device *dev, struct hash_ctx *ctx)
{
	struct hpm_crypto_session *session;

	ARG_UNUSED(dev);

	if ((ctx == NULL) || (ctx->drv_sessn_state == NULL)) {
		return -EINVAL;
	}

	session = ctx->drv_sessn_state;
	hpm_crypto_free_session(dev->data, session);
	ctx->drv_sessn_state = NULL;
	ctx->hash_hndlr = NULL;
	ctx->started = false;

	return 0;
}

static DEVICE_API(crypto, hpm_crypto_api) = {
	.query_hw_caps = hpm_crypto_query_hw_caps,
	.cipher_begin_session = hpm_crypto_cipher_begin_session,
	.cipher_free_session = hpm_crypto_cipher_free_session,
	.hash_begin_session = hpm_crypto_hash_begin_session,
	.hash_free_session = hpm_crypto_hash_free_session,
};

static int hpm_crypto_init(const struct device *dev)
{
	const struct hpm_crypto_config *config = dev->config;
	struct hpm_crypto_data *data = dev->data;
	hpm_stat_t status;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	status = sdp_init(config->base);
	if (status != status_success) {
		LOG_ERR("%s: SDP initialization failed (0x%x)", dev->name, status);
		return hpm_crypto_status_to_errno(status);
	}

	/* The nocache section is not zero-initialized by the C runtime. */
	memset(data, 0, sizeof(*data));
	k_mutex_init(&data->session_lock);
	k_mutex_init(&data->hw_lock);

	return 0;
}

#define HPM_CRYPTO_INIT(n)                                                                         \
	static struct hpm_crypto_data hpm_crypto_data_##n __nocache;                               \
                                                                                                   \
	static const struct hpm_crypto_config hpm_crypto_config_##n = {                            \
		.base = (SDP_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_crypto_init, NULL, &hpm_crypto_data_##n,                      \
			      &hpm_crypto_config_##n, POST_KERNEL, CONFIG_CRYPTO_INIT_PRIORITY,    \
			      &hpm_crypto_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_CRYPTO_INIT)
