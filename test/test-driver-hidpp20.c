/*
 * Copyright © 2026 libratbag contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <config.h>

#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

/*
 * The HID++ 2.0 driver only exports 'struct ratbag_driver hidpp20_driver';
 * the profile read/commit glue exercised here and the driver's private data
 * are all static. libratbag is a static archive, so pulling the driver
 * source in directly lets us drive that glue without a physical mouse: the
 * hidpp20_driver symbol defined through this inclusion satisfies the
 * reference from libratbag.c and the archive's own copy is never linked.
 */
#include "driver-hidpp20.c"

/* Matches the sector size of G402/G502-class devices. The in-memory
 * profile image must be at least as large as the on-device profile layout
 * (union hidpp20_internal_profile) or the serializer would overrun it. */
#define TEST_SECTOR_SIZE 256
#define TEST_NUM_PROFILES 3
#define TEST_NUM_RESOLUTIONS 5

/* size of the raw on-device name field, see struct hidpp20_profile */
#define NAME_FIELD_SIZE 48

static int
open_restricted(const char *path, int flags, void *user_data)
{
	/* we never touch a real device */
	ck_abort();
	return 0;
}

static void
close_restricted(int fd, void *user_data)
{
	/* we never touch a real device */
	ck_abort();
}

static struct ratbag_interface abort_iface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

/**
 * Fill a raw on-device name field with the UTF-16LE encoding of an ASCII
 * string, zero-padded. Done by hand so the tests pin down the actual byte
 * layout instead of trusting the same iconv helper the driver uses.
 */
static void
ascii_to_raw_name(uint8_t raw[NAME_FIELD_SIZE], const char *ascii)
{
	size_t i;

	memset(raw, 0, NAME_FIELD_SIZE);
	ck_assert_uint_le(strlen(ascii) * 2, NAME_FIELD_SIZE);
	for (i = 0; ascii[i]; i++) {
		ck_assert_uint_lt((unsigned char)ascii[i], 0x80);
		raw[2 * i] = (uint8_t)ascii[i];
		raw[2 * i + 1] = 0x00;
	}
}

/**
 * Create a ratbag device bound to the HID++ 2.0 driver, backed by an
 * in-memory 0x8100 (Onboard Profiles) image instead of a mouse. The fake
 * hidpp20_device has no fd and no feature list, so any attempt to talk to
 * the device fails fast without blocking.
 *
 * names[i] is the (ASCII) name stored on the device for profile i, or NULL
 * for a profile that was never named.
 */
static struct ratbag_device *
hidpp20_8100_device_new(struct ratbag *ratbag,
			const char *names[TEST_NUM_PROFILES])
{
	struct input_id id = { 0 };
	struct ratbag_device *device;
	struct hidpp20drv_data *drv_data;
	struct hidpp20_profiles *profiles;
	struct ratbag_profile *profile;
	unsigned int i, j;

	device = ratbag_device_new(ratbag, NULL, "Test HID++ 2.0 device", &id);
	ck_assert_ptr_nonnull(device);
	for (i = 0; i < MAX_HIDRAW; i++)
		device->hidraw[i].fd = -1;
	device->driver = &hidpp20_driver;

	drv_data = zalloc(sizeof(*drv_data));
	ratbag_set_drv_data(device, drv_data);

	drv_data->dev = zalloc(sizeof(*drv_data->dev));
	hidpp_device_init(&drv_data->dev->base, -1);
	hidpp_device_set_log_handler(&drv_data->dev->base, hidpp20_log,
				     HIDPP_LOG_PRIORITY_RAW, device);
	drv_data->dev->index = HIDPP_RECEIVER_IDX;
	drv_data->dev->proto_major = 4;
	drv_data->dev->proto_minor = 2;

	drv_data->capabilities = HIDPP_CAP_ONBOARD_PROFILES_8100;

	drv_data->num_sensors = 1;
	drv_data->sensors = zalloc(sizeof(*drv_data->sensors));
	drv_data->sensors[0].dpi_min = 100;
	drv_data->sensors[0].dpi_max = 16000;
	drv_data->sensors[0].dpi_steps = 50;

	drv_data->report_rates[0] = 1000;
	drv_data->num_report_rates = 1;

	profiles = zalloc(sizeof(*profiles));
	profiles->num_profiles = TEST_NUM_PROFILES;
	profiles->num_rom_profiles = 1;
	profiles->num_modes = TEST_NUM_RESOLUTIONS;
	profiles->sector_size = TEST_SECTOR_SIZE;
	profiles->active_profile_index = 0;
	profiles->profiles = zalloc(TEST_NUM_PROFILES * sizeof(*profiles->profiles));
	for (i = 0; i < TEST_NUM_PROFILES; i++) {
		struct hidpp20_profile *p = &profiles->profiles[i];

		p->address = 0x0001 + i;
		p->enabled = 1;
		p->report_rate = 1000;
		p->default_dpi = 1;
		for (j = 0; j < HIDPP20_DPI_COUNT; j++)
			p->dpi[j] = 400 * (j + 1);
		if (names[i])
			ascii_to_raw_name((uint8_t *)p->name, names[i]);
	}
	drv_data->profiles = profiles;

	drv_data->num_profiles = TEST_NUM_PROFILES;
	drv_data->num_resolutions = TEST_NUM_RESOLUTIONS;
	drv_data->num_buttons = 0;
	drv_data->num_leds = 0;

	/* same as hidpp20drv_init_device() */
	ratbag_device_init_profiles(device,
				    drv_data->num_profiles,
				    drv_data->num_resolutions,
				    drv_data->num_buttons,
				    drv_data->num_leds);
	ratbag_device_for_each_profile(device, profile)
		hidpp20drv_read_profile(profile);

	return device;
}

/**
 * Replace the raw on-device name of profile idx with the given bytes and
 * re-read the profile through the driver, as if the device had reported
 * that name at probe time.
 */
static void
reread_with_raw_name(struct ratbag_device *device, unsigned int idx,
		     const uint8_t raw[NAME_FIELD_SIZE])
{
	struct hidpp20drv_data *drv_data = ratbag_get_drv_data(device);
	struct ratbag_profile *profile;

	memcpy(drv_data->profiles->profiles[idx].name, raw, NAME_FIELD_SIZE);
	profile = ratbag_device_get_profile(device, idx);
	hidpp20drv_read_profile(profile);
	ratbag_profile_unref(profile);
}

static void
assert_profile_name(struct ratbag_device *device, unsigned int idx,
		    const char *expected)
{
	struct ratbag_profile *profile;

	profile = ratbag_device_get_profile(device, idx);
	ck_assert_ptr_nonnull(ratbag_profile_get_name(profile));
	ck_assert_str_eq(ratbag_profile_get_name(profile), expected);
	ratbag_profile_unref(profile);
}

START_TEST(hidpp20_8100_profile_name_read)
{
	const char *names[TEST_NUM_PROFILES] = { "banana", NULL, "kiwi" };
	struct ratbag *r;
	struct ratbag_device *d;

	r = ratbag_create_context(&abort_iface, NULL);
	d = hidpp20_8100_device_new(r, names);

	assert_profile_name(d, 0, "banana");
	assert_profile_name(d, 2, "kiwi");

	ratbag_device_unref(d);
	ratbag_unref(r);
}
END_TEST

START_TEST(hidpp20_8100_profile_name_read_utf16)
{
	const char *names[TEST_NUM_PROFILES] = { NULL, NULL, NULL };
	struct ratbag *r;
	struct ratbag_device *d;
	uint8_t raw[NAME_FIELD_SIZE];
	unsigned int i;

	r = ratbag_create_context(&abort_iface, NULL);
	d = hidpp20_8100_device_new(r, names);

	/* "Zoë", padded with 0xffff the way Logitech's software leaves the
	 * unused part of the field */
	memset(raw, 0xff, sizeof(raw));
	raw[0] = 'Z';  raw[1] = 0x00;
	raw[2] = 'o';  raw[3] = 0x00;
	raw[4] = 0xeb; raw[5] = 0x00; /* U+00EB */
	reread_with_raw_name(d, 0, raw);
	assert_profile_name(d, 0, "Zo\xc3\xab");

	/* "名前" (U+540D U+524D), non-Latin BMP characters, zero padded */
	memset(raw, 0, sizeof(raw));
	raw[0] = 0x0d; raw[1] = 0x54;
	raw[2] = 0x4d; raw[3] = 0x52;
	reread_with_raw_name(d, 1, raw);
	assert_profile_name(d, 1, "\xe5\x90\x8d\xe5\x89\x8d");

	/* a name filling all 24 code units, i.e. without any terminator */
	for (i = 0; i < NAME_FIELD_SIZE; i += 2) {
		raw[i] = 'x';
		raw[i + 1] = 0x00;
	}
	reread_with_raw_name(d, 2, raw);
	assert_profile_name(d, 2, "xxxxxxxxxxxxxxxxxxxxxxxx");

	/* an undecodable name (lone high surrogate) must not leak through
	 * as garbage, nor make the profile un-nameable */
	memset(raw, 0, sizeof(raw));
	raw[0] = 0x00; raw[1] = 0xd8; /* U+D800 */
	raw[2] = 'a';  raw[3] = 0x00;
	reread_with_raw_name(d, 1, raw);
	assert_profile_name(d, 1, "Profile 2");

	ratbag_device_unref(d);
	ratbag_unref(r);
}
END_TEST

START_TEST(hidpp20_8100_profile_name_default)
{
	const char *names[TEST_NUM_PROFILES] = { NULL, NULL, NULL };
	struct ratbag *r;
	struct ratbag_device *d;
	uint8_t raw[NAME_FIELD_SIZE];
	unsigned int i;

	r = ratbag_create_context(&abort_iface, NULL);
	d = hidpp20_8100_device_new(r, names);

	/* Every 0x8100 profile carries a name field, so the name is always
	 * writable. A profile that was never named must not end up with a
	 * NULL (unsupported) or empty (read-only) name, otherwise the user
	 * can never assign one. Mirror the HID++ 1.0 driver and fall back to
	 * "Profile N". */
	for (i = 0; i < TEST_NUM_PROFILES; i++) {
		char expected[16];

		snprintf(expected, sizeof(expected), "Profile %u", i + 1);
		assert_profile_name(d, i, expected);
	}

	/* same for the raw never-named representation (all 0xff), in case
	 * it makes it past the sector parser */
	memset(raw, 0xff, sizeof(raw));
	reread_with_raw_name(d, 1, raw);
	assert_profile_name(d, 1, "Profile 2");

	ratbag_device_unref(d);
	ratbag_unref(r);
}
END_TEST

START_TEST(hidpp20_8100_profile_name_set)
{
	const char *names[TEST_NUM_PROFILES] = { "banana", NULL, NULL };
	struct ratbag *r;
	struct ratbag_device *d;
	struct ratbag_profile *p;
	int rc;

	r = ratbag_create_context(&abort_iface, NULL);
	d = hidpp20_8100_device_new(r, names);

	/* a profile with a name stored on the device */
	p = ratbag_device_get_profile(d, 0);
	rc = ratbag_profile_set_name(p, "cherry");
	ck_assert_int_eq(rc, RATBAG_SUCCESS);
	ck_assert_str_eq(ratbag_profile_get_name(p), "cherry");
	ratbag_profile_unref(p);

	/* a profile that was never named on the device, this is the
	 * reproducer of https://github.com/libratbag/libratbag/issues/680:
	 * "ratbagctl <device> profile 0 name set blah" must not fail with
	 * RATBAG_ERROR_CAPABILITY on a G502 */
	p = ratbag_device_get_profile(d, 1);
	rc = ratbag_profile_set_name(p, "blah");
	ck_assert_int_eq(rc, RATBAG_SUCCESS);
	ck_assert_str_eq(ratbag_profile_get_name(p), "blah");
	ratbag_profile_unref(p);

	ratbag_device_unref(d);
	ratbag_unref(r);
}
END_TEST

START_TEST(hidpp20_8100_profile_name_commit)
{
	const char *names[TEST_NUM_PROFILES] = { "banana", NULL, "kiwi" };
	struct ratbag *r;
	struct ratbag_device *d;
	struct ratbag_profile *p;
	struct hidpp20drv_data *drv_data;
	uint8_t expected[NAME_FIELD_SIZE];
	int rc;

	r = ratbag_create_context(&abort_iface, NULL);
	d = hidpp20_8100_device_new(r, names);
	drv_data = ratbag_get_drv_data(d);

	p = ratbag_device_get_profile(d, 0);
	rc = ratbag_profile_set_name(p, "cherry");
	ck_assert_int_eq(rc, RATBAG_SUCCESS);
	ratbag_profile_unref(p);

	p = ratbag_device_get_profile(d, 1);
	rc = ratbag_profile_set_name(p, "blah");
	ck_assert_int_eq(rc, RATBAG_SUCCESS);
	ratbag_profile_unref(p);

	/* The names must be encoded into the onboard profile image before it
	 * is serialized and sent to the device. Without a device the final
	 * write fails (and the driver logs an error), but the image must
	 * already carry the new names by then. */
	rc = hidpp20drv_commit(d);
	ck_assert_int_ne(rc, RATBAG_ERROR_CAPABILITY);

	ascii_to_raw_name(expected, "cherry");
	ck_assert_mem_eq(drv_data->profiles->profiles[0].name, expected, NAME_FIELD_SIZE);
	ascii_to_raw_name(expected, "blah");
	ck_assert_mem_eq(drv_data->profiles->profiles[1].name, expected, NAME_FIELD_SIZE);
	/* a profile that was not touched keeps its on-device name bytes */
	ascii_to_raw_name(expected, "kiwi");
	ck_assert_mem_eq(drv_data->profiles->profiles[2].name, expected, NAME_FIELD_SIZE);

	ratbag_device_unref(d);
	ratbag_unref(r);
}
END_TEST

START_TEST(hidpp20_8100_profile_name_commit_utf16)
{
	const char *names[TEST_NUM_PROFILES] = { NULL, NULL, NULL };
	struct ratbag *r;
	struct ratbag_device *d;
	struct ratbag_profile *p;
	struct hidpp20drv_data *drv_data;
	uint8_t expected[NAME_FIELD_SIZE];
	unsigned int i;
	int rc;

	r = ratbag_create_context(&abort_iface, NULL);
	d = hidpp20_8100_device_new(r, names);
	drv_data = ratbag_get_drv_data(d);

	/* non-ASCII name */
	p = ratbag_device_get_profile(d, 0);
	rc = ratbag_profile_set_name(p, "Zo\xc3\xab");
	ck_assert_int_eq(rc, RATBAG_SUCCESS);
	ratbag_profile_unref(p);

	/* a name longer than the 24 code units the device can store */
	p = ratbag_device_get_profile(d, 1);
	rc = ratbag_profile_set_name(p, "abcdefghijklmnopqrstuvwxyz0123");
	ck_assert_int_eq(rc, RATBAG_SUCCESS);
	ratbag_profile_unref(p);

	/* profile 2 was never named and is left untouched: its made-up
	 * default name must not be written to the device */
	assert_profile_name(d, 2, "Profile 3");

	rc = hidpp20drv_commit(d);
	ck_assert_int_ne(rc, RATBAG_ERROR_CAPABILITY);

	memset(expected, 0, sizeof(expected));
	expected[0] = 'Z';  expected[1] = 0x00;
	expected[2] = 'o';  expected[3] = 0x00;
	expected[4] = 0xeb; expected[5] = 0x00;
	ck_assert_mem_eq(drv_data->profiles->profiles[0].name, expected, NAME_FIELD_SIZE);

	/* truncated to the first 24 characters, nothing written past the
	 * field */
	for (i = 0; i < NAME_FIELD_SIZE; i += 2) {
		expected[i] = 'a' + i / 2;
		expected[i + 1] = 0x00;
	}
	ck_assert_mem_eq(drv_data->profiles->profiles[1].name, expected, NAME_FIELD_SIZE);

	/* untouched profile: still unnamed on the device */
	memset(expected, 0, sizeof(expected));
	ck_assert_mem_eq(drv_data->profiles->profiles[2].name, expected, NAME_FIELD_SIZE);

	ratbag_device_unref(d);
	ratbag_unref(r);
}
END_TEST

static Suite *
test_driver_hidpp20_suite(void)
{
	TCase *tc;
	Suite *s;

	s = suite_create("driver-hidpp20");
	tc = tcase_create("onboard profiles (0x8100)");
	tcase_add_test(tc, hidpp20_8100_profile_name_read);
	tcase_add_test(tc, hidpp20_8100_profile_name_read_utf16);
	tcase_add_test(tc, hidpp20_8100_profile_name_default);
	tcase_add_test(tc, hidpp20_8100_profile_name_set);
	tcase_add_test(tc, hidpp20_8100_profile_name_commit);
	tcase_add_test(tc, hidpp20_8100_profile_name_commit_utf16);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s;
	SRunner *sr;
	const struct rlimit corelimit = { 0, 0 };

	setrlimit(RLIMIT_CORE, &corelimit);

	s = test_driver_hidpp20_suite();
	sr = srunner_create(s);

	srunner_run_all(sr, CK_ENV);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);

	return (nfailed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
