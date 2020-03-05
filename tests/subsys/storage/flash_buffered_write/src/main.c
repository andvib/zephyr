/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <zephyr/types.h>
#include <stdbool.h>
#include <ztest.h>
#include <drivers/flash.h>

#include <storage/fbw.h>

#define BUF_LEN 512
#define MAX_PAGE_SIZE 0x1000 /* Max supported page size to run test on */
#define MAX_NUM_PAGES 4      /* Max number of pages used in these tests */
#define TESTBUF_SIZE (MAX_PAGE_SIZE * MAX_NUM_PAGES)
#define FLASH_SIZE DT_SOC_NV_FLASH_0_SIZE
#define FLASH_NAME DT_FLASH_DEV_NAME

/* so that we don't overwrite the application when running on hw */
#define FLASH_BASE (64*1024)
#define FLASH_AVAILABLE (FLASH_SIZE-FLASH_BASE)

static struct device *fdev;
static const struct flash_driver_api *api;
static const struct flash_pages_layout *layout;
static size_t layout_size;
static struct fbw_ctx ctx;
static int page_size;
static u8_t *cb_buf;
static size_t cb_len;
static size_t cb_offset;
static int cb_ret;

static u8_t buf[BUF_LEN];
static u8_t read_buf[TESTBUF_SIZE];
const static u8_t write_buf[TESTBUF_SIZE] = {[0 ... TESTBUF_SIZE - 1] = 0xaa};
static u8_t written_pattern[TESTBUF_SIZE] = {[0 ... TESTBUF_SIZE - 1] = 0xaa};
static u8_t erased_pattern[TESTBUF_SIZE]  = {[0 ... TESTBUF_SIZE - 1] = 0xff};

#define VERIFY_BUF(start, size, buf) \
do { \
	rc = flash_read(fdev, FLASH_BASE + start, read_buf, size); \
	zassert_equal(rc, 0, "should succeed"); \
	zassert_mem_equal(read_buf, buf, size, "should equal %s", #buf);\
} while (0)

#define VERIFY_WRITTEN(start, size) VERIFY_BUF(start, size, written_pattern)
#define VERIFY_ERASED(start, size) VERIFY_BUF(start, size, erased_pattern)

int fbw_callback(u8_t *buf, size_t len, size_t offset)
{
	if (cb_buf) {
		zassert_equal(cb_buf, buf, "incorrect buf");
		zassert_equal(cb_len, len, "incorrect length");
		zassert_equal(cb_offset, offset, "incorrect offset");
	}

	return cb_ret;
}

static void erase_flash(void)
{
	int rc;

	rc = flash_write_protection_set(fdev, false);
	zassert_equal(rc, 0, "should succeed");

	for (int i = 0; i < MAX_NUM_PAGES; i++) {
		rc = flash_erase(fdev,
				 FLASH_BASE + (i * layout->pages_size),
				 layout->pages_size);
		zassert_equal(rc, 0, "should succeed");
	}

	rc = flash_write_protection_set(fdev, true);
	zassert_equal(rc, 0, "should succeed");
}


static void init_target(void)
{
	int rc;

	/* Ensure that target is clean */
	memset(&ctx, 0, sizeof(ctx));
	memset(buf, 0, BUF_LEN);

	/* Disable callback tests */
	cb_len = 0;
	cb_offset = 0;
	cb_buf = NULL;
	cb_ret = 0;

	erase_flash();

	rc = fbw_init(&ctx, fdev, buf, BUF_LEN, FLASH_BASE, 0, fbw_callback);
	zassert_equal(rc, 0, "expected success");
}

static void test_fbw_init(void)
{
	int rc;

	init_target();

	/* End address out of range */
	rc = fbw_init(&ctx, fdev, buf, BUF_LEN, FLASH_BASE,
		      FLASH_AVAILABLE + 4, NULL);
	zassert_true(rc < 0, "should fail as size is more than available");

	rc = fbw_init(NULL, fdev, buf, BUF_LEN, FLASH_BASE, 0, NULL);
	zassert_true(rc < 0, "should fail as ctx is NULL");

	rc = fbw_init(&ctx, NULL, buf, BUF_LEN, FLASH_BASE, 0, NULL);
	zassert_true(rc < 0, "should fail as fdev is NULL");

	rc = fbw_init(&ctx, fdev, NULL, BUF_LEN, FLASH_BASE, 0, NULL);
	zassert_true(rc < 0, "should fail as buffer is NULL");

	/* Entering '0' as flash size uses rest of flash. */
	rc = fbw_init(&ctx, fdev, buf, BUF_LEN, FLASH_BASE, 0, NULL);
	zassert_equal(rc, 0, "should succeed");
	zassert_equal(FLASH_AVAILABLE, ctx.available, "Wrong size");
}

static void test_fbw_write(void)
{
	int rc;

	init_target();

	/* Don't fill up the buffer */
	rc = fbw_write(&ctx, write_buf, BUF_LEN - 1, false);
	zassert_equal(rc, 0, "expected success");

	/* Verify that no data has been written */
	VERIFY_ERASED(0, BUF_LEN);

	/* Now, write the missing byte, which should trigger a dump to flash */
	rc = fbw_write(&ctx, write_buf, 1, false);
	zassert_equal(rc, 0, "expected success");

	VERIFY_WRITTEN(0, BUF_LEN);
}

static void test_fbw_write_cross_buf_border(void)
{
	int rc;

	init_target();

	/* Test when write crosses border of the buffer */
	rc = fbw_write(&ctx, write_buf, BUF_LEN + 128, false);
	zassert_equal(rc, 0, "expected success");

	/* 1xBuffer should be dumped to flash */
	VERIFY_WRITTEN(0, BUF_LEN);

	/* Fill rest of the buffer */
	rc = fbw_write(&ctx, write_buf, BUF_LEN - 128, false);
	zassert_equal(rc, 0, "expected success");
	VERIFY_WRITTEN(BUF_LEN, BUF_LEN);

	/* Fill half of the buffer */
	rc = fbw_write(&ctx, write_buf, BUF_LEN/2, false);
	zassert_equal(rc, 0, "expected success");

	/* Flush the buffer */
	rc = fbw_write(&ctx, write_buf, 0, true);
	zassert_equal(rc, 0, "expected success");

	/* Two and a half buffers should be written */
	VERIFY_WRITTEN(0, BUF_LEN * 2 + BUF_LEN / 2);
}

static void test_fbw_write_multi_page(void)
{
	int rc;
	int num_pages = MAX_NUM_PAGES - 1;

	init_target();

	/* Test when write spans multiple pages crosses border of page */
	rc = fbw_write(&ctx, write_buf, (page_size * num_pages) + 128, false);
	zassert_equal(rc, 0, "expected success");

	/* First three pages should be written */
	VERIFY_WRITTEN(0, page_size * num_pages);

	/* Fill rest of the page */
	rc = fbw_write(&ctx, write_buf, page_size - 128, false);
	zassert_equal(rc, 0, "expected success");

	/* First four pages should be written */
	VERIFY_WRITTEN(0, BUF_LEN * (num_pages + 1));
}

static void test_fbw_bytes_written(void)
{
	int rc;
	size_t offset;

	init_target();

	/* Verify that the offset is retained across failed downolads */
	rc = fbw_write(&ctx, write_buf, BUF_LEN + 128, false);
	zassert_equal(rc, 0, "expected success");

	/* First page should be written */
	VERIFY_WRITTEN(0, BUF_LEN);

	/* Fill rest of the page */
	offset = fbw_bytes_written(&ctx);
	zassert_equal(offset, BUF_LEN, "offset should match buf size");

	/* Fill up the buffer MINUS 128 to verify that write_buf_pos is kept */
	rc = fbw_write(&ctx, write_buf, BUF_LEN - 128, false);
	zassert_equal(rc, 0, "expected success");

	/* Second page should be written */
	VERIFY_WRITTEN(BUF_LEN, BUF_LEN);
}

static void test_fbw_buf_size_greater_than_page_size(void)
{
	int rc;

	/* To illustrate that other params does not trigger error */
	rc = fbw_init(&ctx, fdev, buf, 0x10, 0, 0, NULL);
	zassert_equal(rc, 0, "expected success");

	/* Only change buf_len param */
	rc = fbw_init(&ctx, fdev, buf, 0x10000, 0, 0, NULL);
	zassert_true(rc < 0, "expected failure");
}

static void test_fbw_write_callback(void)
{
	int rc;

	init_target();

	/* Trigger verification in callback */
	cb_buf = buf;
	cb_len = BUF_LEN;
	cb_offset = FLASH_BASE;

	rc = fbw_write(&ctx, write_buf, BUF_LEN + 128, false);
	zassert_equal(rc, 0, "expected success");

	cb_len = BUF_LEN;
	cb_offset = FLASH_BASE + BUF_LEN;

	/* Fill rest of the buffer */
	rc = fbw_write(&ctx, write_buf, BUF_LEN - 128, false);
	zassert_equal(rc, 0, "expected success");
	VERIFY_WRITTEN(BUF_LEN, BUF_LEN);

	/* Fill half of the buffer and flush it to flash */
	cb_len = BUF_LEN/2;
	cb_offset = FLASH_BASE + (2 * BUF_LEN);

	rc = fbw_write(&ctx, write_buf, BUF_LEN/2, true);
	zassert_equal(rc, 0, "expected success");

	/* Ensure that failing callback trickles up to caller */
	cb_ret = -EFAULT;
	cb_buf = NULL; /* Don't verify other parameters of the callback */
	rc = fbw_write(&ctx, write_buf, BUF_LEN, true);
	zassert_equal(rc, -EFAULT, "expected failure from callback");
}

#ifdef CONFIG_FBW_ERASE
static void test_fbw_erase(void)
{
	int rc;

	init_target();

	/* Write out one buf */
	rc = fbw_write(&ctx, write_buf, BUF_LEN, false);
	zassert_equal(rc, 0, "expected success");

	rc = fbw_erase(&ctx, FLASH_BASE);
	zassert_equal(rc, 0, "expected success");

	VERIFY_ERASED(FLASH_BASE, page_size);
}
#endif

void test_main(void)
{
	fdev = device_get_binding(FLASH_NAME);
	api = fdev->driver_api;
	api->page_layout(fdev, &layout, &layout_size);

	page_size = layout->pages_size;
	__ASSERT_NO_MSG(page_size > BUF_LEN);

	ztest_test_suite(lib_fbw_test,
	     ztest_unit_test(test_fbw_init),
	     ztest_unit_test(test_fbw_write),
	     ztest_unit_test(test_fbw_write_cross_buf_border),
	     ztest_unit_test(test_fbw_write_multi_page),
	     ztest_unit_test(test_fbw_buf_size_greater_than_page_size),
	     ztest_unit_test(test_fbw_write_callback),
#ifdef CONFIG_FBW_ERASE
	     ztest_unit_test(test_fbw_erase),
#endif
	     ztest_unit_test(test_fbw_bytes_written)
	 );

	ztest_run_test_suite(lib_fbw_test);
}
