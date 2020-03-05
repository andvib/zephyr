/*
 * Copyright (c) 2017, 2020 Nordic Semiconductor ASA
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for flash buffered writes
 */

#ifndef ZEPHYR_INCLUDE_STORAGE_FBW_H_
#define ZEPHYR_INCLUDE_STORAGE_FBW_H_

/**
 * @brief Abstraction over flash buffered writes.
 *
 * @defgroup fbw flash buffered write Interface
 * @{
 */

#include <stdbool.h>
#include <drivers/flash.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @typedef fbw_callback_t
 *
 * @details The fbw callback is called when data from the buffer is written
 * to flash. Once the flash_write operation completes, a flash_read operation
 * is used to read the data back out. This enables verifying that the data
 * has been correctly stored (for instance by using a SHA function).
 * The write buffer 'buf' provided in fbw_init is used as a read buffer for
 * this purpose.
 *
 * @param buf Pointer to the data read.
 * @param len The length of the data read.
 * @param offset The offset the data was read from.
 */
typedef int (*fbw_callback_t)(u8_t *buf, size_t len, size_t offset);

/**
 * @brief Structure for flash buffered writes
 *
 * Users should treat these structures as opaque values and only interact
 * with them through the below API.
 */
struct fbw_ctx {
	u8_t *buf; /* Write buffer */
	size_t buf_len; /* Length of write buffer */
	size_t buf_bytes; /* Number of bytes currently stored in write buf */
	struct device *fdev; /* Flash device */
	size_t bytes_written; /* Number of bytes written to flash */
	size_t offset; /* Offset from base of flash device to write area */
	size_t available; /* Available bytes in write area */
	fbw_callback_t callback; /* Callback invoked on completed write op */
#ifdef CONFIG_FBW_ERASE
	off_t last_erased_page_start_offset; /* Last erased offset */
#endif
};

/**
 * @brief Initialize context needed for buffered write to flash.
 *
 * @param ctx context to be initialized
 * @param fdev Flash device to operate on
 * @param buf Write buffer
 * @param buf_len Length of write buffer. Can not be larger than the page size
 * @param offset Offset within flash device to start writing to
 * @param size Number of bytes available for performing buffered write.
 *             If this is '0', the size will be set to the total size
 *             of the flash device minus the offset.
 * @param cb Callback to be invoked on completed flash write operations.
 *
 * @return non-negative on success, negative errno code on fail
 */
int fbw_init(struct fbw_ctx *ctx, struct device *fdev, u8_t *buf,
	     size_t buf_len, size_t offset, size_t size, fbw_callback_t cb);
/**
 * @brief Read number of bytes written to the flash.
 *
 * @note api-tags: pre-kernel-ok isr-ok
 *
 * @param ctx context
 *
 * @return Number of bytes written to flash.
 */
size_t fbw_bytes_written(struct fbw_ctx *ctx);

/**
 * @brief  Process input buffers to be written to flash device in single blocks.
 * Will store remainder between calls.
 *
 * A final call to this function with flush set to true
 * will write out the remaining block buffer to flash.
 *
 * @param ctx context
 * @param data data to write
 * @param len Number of bytes to write
 * @param flush when true this forces any buffered data to be written to flash
 *
 * @return non-negative on success, negative errno code on fail
 */
int fbw_write(struct fbw_ctx *ctx, const u8_t *data, size_t len, bool flush);

/**
 * @brief Erase the flash page to which a given offset belongs.
 *
 * This function erases a flash page to which an offset belongs if this page
 * is not the page previously erased by the provided ctx
 * (ctx->last_erased_page_start_offset).
 *
 * @param ctx context
 * @param off offset from the base address of the flash device
 *
 * @return non-negative on success, negative errno code on fail
 */
int fbw_erase(struct fbw_ctx *ctx, off_t off);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* ZEPHYR_INCLUDE_STORAGE_FBW_H_ */
