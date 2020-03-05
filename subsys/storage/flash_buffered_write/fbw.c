/*
 * Copyright (c) 2017, 2020 Nordic Semiconductor ASA
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME FBW
#define LOG_LEVEL CONFIG_FBW_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME, CONFIG_FBW_LOG_LEVEL);

#include <zephyr/types.h>
#include <string.h>
#include <drivers/flash.h>

#include <storage/fbw.h>

#ifdef CONFIG_FBW_ERASE

int fbw_erase(struct fbw_ctx *ctx, off_t off)
{
	int rc;
	struct flash_pages_info page;

	rc = flash_get_page_info_by_offs(ctx->fdev, off, &page);
	if (rc != 0) {
		LOG_ERR("Error %d while getting page info", rc);
		return rc;
	}

	if (ctx->last_erased_page_start_offset == page.start_offset) {
		return 0;
	}

	ctx->last_erased_page_start_offset = page.start_offset;
	LOG_INF("Erasing page at offset 0x%08lx", (long)page.start_offset);

	flash_write_protection_set(ctx->fdev, false);
	rc = flash_erase(ctx->fdev, page.start_offset, page.size);
	flash_write_protection_set(ctx->fdev, true);

	if (rc != 0) {
		LOG_ERR("Error %d while erasing page", rc);
	}

	return rc;
}

#endif /* CONFIG_FBW_ERASE */

static int flash_sync(struct fbw_ctx *ctx)
{
	int rc = 0;
	size_t write_addr = ctx->offset + ctx->bytes_written;


	if (IS_ENABLED(CONFIG_FBW_ERASE)) {
		rc = fbw_erase(ctx, write_addr + ctx->buf_bytes);
		if (rc < 0) {
			LOG_ERR("fbw_erase error %d offset=0x%08zx", rc,
					write_addr);
			return rc;
		}
	}

	flash_write_protection_set(ctx->fdev, false);
	rc = flash_write(ctx->fdev, write_addr, ctx->buf, ctx->buf_bytes);
	flash_write_protection_set(ctx->fdev, true);

	if (rc != 0) {
		LOG_ERR("flash_write error %d offset=0x%08zx", rc,
			write_addr);
		return rc;
	}

	if (ctx->callback) {
		rc = flash_read(ctx->fdev, write_addr, ctx->buf,
				ctx->buf_bytes);
		if (rc != 0) {
			LOG_ERR("flash read failed: %d", rc);
			return rc;
		}

		rc = ctx->callback(ctx->buf, ctx->buf_bytes, write_addr);
		if (rc != 0) {
			LOG_ERR("callback failed: %d", rc);
		}
	}

	ctx->bytes_written += ctx->buf_bytes;
	ctx->buf_bytes = 0U;

	return rc;
}

int fbw_write(struct fbw_ctx *ctx, const u8_t *data, size_t len, bool flush)
{
	int processed = 0;
	int rc = 0;
	int buf_empty_bytes;

	if (!ctx || !data) {
		return -EFAULT;
	}

	if (ctx->bytes_written + ctx->buf_bytes + len > ctx->available) {
		return -ENOMEM;
	}

	while ((len - processed) >=
	       (buf_empty_bytes = ctx->buf_len - ctx->buf_bytes)) {
		memcpy(ctx->buf + ctx->buf_bytes, data + processed,
		       buf_empty_bytes);

		ctx->buf_bytes = ctx->buf_len;
		rc = flash_sync(ctx);

		if (rc != 0) {
			return rc;
		}

		processed += buf_empty_bytes;
	}

	/* place rest of the data into ctx->buf */
	if (processed < len) {
		memcpy(ctx->buf + ctx->buf_bytes,
		       data + processed, len - processed);
		ctx->buf_bytes += len - processed;
	}

	if (flush && ctx->buf_bytes > 0) {
		rc = flash_sync(ctx);
	}

	return rc;
}

size_t fbw_bytes_written(struct fbw_ctx *ctx)
{
	return ctx->bytes_written;
}

int fbw_init(struct fbw_ctx *ctx, struct device *fdev, u8_t *buf,
	     size_t buf_len, size_t offset, size_t size, fbw_callback_t cb)
{
	if (!ctx || !fdev || !buf) {
		return -EFAULT;
	}

	size_t layout_size = 0;
	size_t total_size = 0;
	const struct flash_pages_layout *layout;
	const struct flash_driver_api *api = fdev->driver_api;

	/* Calculate the total size of the flash device */
	api->page_layout(fdev, &layout, &layout_size);
	for (int i = 0; i < layout_size; i++) {

		total_size += layout->pages_count * layout->pages_size;

		if (buf_len > layout->pages_size) {
			LOG_ERR("Buffer size is bigger than page");
			return -EFAULT;
		}

		layout++;

	}

	if ((offset + size) > total_size ||
	    offset % api->write_block_size) {
		LOG_ERR("Incorrect parameter");
		return -EFAULT;
	}

	ctx->fdev = fdev;
	ctx->buf = buf;
	ctx->buf_len = buf_len;
	ctx->bytes_written = 0;
	ctx->buf_bytes = 0U;
	ctx->offset = offset;
	ctx->available = (size == 0 ? total_size - offset : size);
	ctx->callback = cb;

#ifdef CONFIG_FBW_ERASE
	ctx->last_erased_page_start_offset = -1;
#endif

	return 0;
}
