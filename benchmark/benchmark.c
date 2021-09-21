#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <zlib.h>
#include "measurement.h"

/*
  STAT1 - This yellow indicator LED is attached to ATmega328 PD5 and
  blinks when UART communication is functioning.

  STAT2 - This green LED is connected to SPI serial clock line.
  This LED only blinks when the SPI interface is active.
  You will see it flash when the OpenLog receives bytes to the microSD card.
 */

#define PACKAGE_VERSION "0.0.1"
MSRT_DECLARE(openlog_data_block);

#define BPS_STR(bps)		      \
	bps == B2400    ? "2400"    : \
	bps == B4800    ? "4800"    : \
	bps == B9600    ? "9600"    : \
	bps == B19200   ? "19200"   : \
	bps == B38400   ? "38400"   : \
	bps == B57600   ? "57600"   : \
	bps == B115200  ? "115200"  : \
	bps == B230400  ? "230400"  : \
	bps == B460800  ? "460800"  : \
	bps == B500000  ? "500000"  : \
	bps == B576000  ? "576000"  : \
	bps == B921600  ? "921600"  : \
	bps == B1000000 ? "1000000" : \
	bps == B1500000 ? "1500000" : \
	bps == B2000000 ? "2000000" : "UNKNOWN"

#define STR_BPS(s)				 \
	!strncmp(s, "2400", 4)    ? (B2400)    : \
	!strncmp(s, "4800", 4)    ? (B4800)    : \
	!strncmp(s, "9600", 4)    ? (B9600)    : \
	!strncmp(s, "19200", 5)   ? (B19200)   : \
	!strncmp(s, "38400", 5)   ? (B38400)   : \
	!strncmp(s, "57600", 5)   ? (B57600)   : \
	!strncmp(s, "115200", 6)  ? (B115200)  : \
	!strncmp(s, "230400", 6)  ? (B230400)  : \
	!strncmp(s, "460800", 6)  ? (B460800)  : \
	!strncmp(s, "500000", 6)  ? (B500000)  : \
	!strncmp(s, "576000", 6)  ? (B576000)  : \
	!strncmp(s, "921600", 6)  ? (B921600)  : \
	!strncmp(s, "1000000", 7) ? (B1000000) : \
	!strncmp(s, "1500000", 7) ? (B1500000) : \
	!strncmp(s, "2000000", 7) ? (B2000000) : (-1)

#define BPS_NUM(bps)		      \
	bps == B2400    ? (2400)    : \
	bps == B4800    ? (4800)    : \
	bps == B9600    ? (9600)    : \
	bps == B19200   ? (19200)   : \
	bps == B38400   ? (38400)   : \
	bps == B57600   ? (57600)   : \
	bps == B115200  ? (115200)  : \
	bps == B230400  ? (230400)  : \
	bps == B460800  ? (460800)  : \
	bps == B500000  ? (500000)  : \
	bps == B576000  ? (576000)  : \
	bps == B921600  ? (921600)  : \
	bps == B1000000 ? (1000000) : \
	bps == B1500000 ? (1500000) : \
	bps == B2000000 ? (2000000) : (-1)

struct options {
	char o_dev_name[PATH_MAX + 1];
	speed_t o_dev_speed;
	uint16_t o_num_data;
	uint32_t o_size_data;
};

struct options opt = {
	.o_dev_name = "/dev/ttyUSB0",
	.o_dev_speed = B2000000,
	.o_num_data = 1,
	.o_size_data = 0xfffff	/* 1MiB. */
};

static uint16_t crc16_update(uint16_t crc, uint8_t a)
{
        int i;

        crc ^= a;
        for (i = 0; i < 8; ++i) {
		if (crc & 1)
			crc = (crc >> 1) ^ 0xA001;
		else
			crc = (crc >> 1);
        }

        return crc;
}

static void usage(const char *cmd_name, const int rc)
{
	fprintf(stdout, "usage: %s [options]\n"
		"\t-d, --device <string> [default: '%s']\n"
		"\t\t""tty device\n"
		"\t-s, --speed <uint> [default: '%s']\n"
		"\t\t""baud speed measured in bits per second\n"
		"\t-n, --number <uint> [default: '%d']\n"
		"\t\t""number of data blocks transmitting,\n"
		"\t\t""each data blocks creates a separate file on openlog device\n"
		"\t-z, --size <uint> [default '%u']\n"
		"\t\t""size of data blocks transmitting in bytes\n"
		"\t-h, --help\n"
		"\t\t""show this help\n"
		"version: %s © 2021 by Thomas Stibor <thomas@stibor.net>\n",
		cmd_name,
		opt.o_dev_name,
		BPS_STR(opt.o_dev_speed),
		opt.o_num_data,
		opt.o_size_data,
		PACKAGE_VERSION);

	exit(rc);
}

static int parse_valid_num(const char *str, long int *val)
{
	char *end = NULL;

	*val = strtol(str, &end, 10);
	if (*end != '\0' || *val < 0)
		return -ERANGE;

	return 0;
}

static int parseopts(int argc, char *argv[])
{
	struct option long_opts[] = {
		{.name = "device", .has_arg = required_argument, .flag = NULL, .val = 'd'},
		{.name = "speed",  .has_arg = required_argument, .flag = NULL, .val = 's'},
		{.name = "number", .has_arg = required_argument, .flag = NULL, .val = 'n'},
		{.name = "size",   .has_arg = required_argument, .flag = NULL, .val = 'z'},
		{.name = "help",   .has_arg = no_argument,	 .flag = NULL, .val = 'h'},
		{.name = NULL}
	};

	int c, rc = 0;
	optind = 0;
	long int val;

	while ((c = getopt_long(argc, argv, "d:s:n:z:h",
				long_opts, NULL)) != -1) {
		switch (c) {
		case 'd': {
			strncpy(opt.o_dev_name, optarg, PATH_MAX);
			break;
		}
                case 's': {
			rc = parse_valid_num(optarg, &val);
                        if (rc || val < 0 || val > UINT32_MAX || (STR_BPS(optarg)) == -1) {
				fprintf(stderr, "argument -s, --speed '%s' out of range or invalid\n",
					optarg);
				return -EINVAL;
                        }
			opt.o_dev_speed = STR_BPS(optarg);
			break;
                }
                case 'n': {
			rc = parse_valid_num(optarg, &val);
                        if (rc || val < 0 || val > UINT16_MAX) {
				fprintf(stderr, "argument -n, --number '%s' out of range\n",
					optarg);
				return -EINVAL;
                        }
			opt.o_num_data = val;
			break;
                }
                case 'z': {
			rc = parse_valid_num(optarg, &val);
                        if (rc || val < 0 || val > UINT32_MAX) {
				fprintf(stderr, "argument -z, --size '%s' out of range\n",
					optarg);
				return -EINVAL;
                        }
			opt.o_size_data = val;
			break;
                }
		case 'h': {
			usage(argv[0], 0);
			break;
		}
                case 0: {
			break;
                }
                default:
			return -EINVAL;
                }
        }

        return rc;
}

int write_data(uint16_t n)
{
	int rc, fd;
	struct termios termios;
	uint32_t crc32sum = 0;
	uint16_t crc16sum = 0;
	uint8_t *buf = NULL;
	ssize_t transfered = 0;

	fd = open(opt.o_dev_name, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "[rc = %d, %s] open device '%s' failed\n",
			errno, strerror(errno), opt.o_dev_name);
		rc = -errno;
		goto cleanup;
	}

	memset(&termios, 0, sizeof(termios));
	rc = tcgetattr(fd, &termios);
	if (rc < 0) {
		fprintf(stderr, "[fd = %d, rc = %d, %s] tcgetattr failed\n",
			fd, errno, strerror(errno));
		rc = -errno;
		goto cleanup;
	}

	rc = cfsetispeed(&termios, opt.o_dev_speed);
	if (rc) {
		fprintf(stderr, "[fd = %d, rc = %d, %s] cfsetispeed '%s' failed\n",
			fd, errno, strerror(errno), BPS_STR(opt.o_dev_speed));
		rc = -errno;
		goto cleanup;
	}

	rc = cfsetospeed(&termios, opt.o_dev_speed);
	if (rc) {
		fprintf(stderr, "[fd = %d, rc = %d, %s] cfsetospeed '%s' failed\n",
			fd, errno, strerror(errno), BPS_STR(opt.o_dev_speed));
		rc = -errno;
		goto cleanup;
	}

	termios.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG | IEXTEN);
	termios.c_oflag &= ~(OPOST | ONLCR);
	termios.c_iflag &= ~(INLCR | ICRNL | IXON | IXOFF | IXANY | IMAXBEL);
	termios.c_cflag &= ~(CSIZE | PARENB);
	termios.c_cflag |= CS8;
	termios.c_cc[VMIN]  = 0;
	termios.c_cc[VTIME] = 1;

	rc = tcsetattr(fd, TCSANOW, &termios);
        if (rc) {
		fprintf(stderr, "[fd = %d, rc = %d, %s] tcsetattr failed\n",
			fd, errno, strerror(errno));
		rc = -errno;
		goto cleanup;
        }

	buf = malloc(sizeof(uint8_t) * opt.o_size_data);
	if (!buf) {
		fprintf(stderr, "[fd = %d, rc = %d, %s] malloc failed\n",
			fd, errno, strerror(errno));
		rc = -errno;
		goto cleanup;
        }
	for (size_t r = 0; r < opt.o_size_data; r++) {
		buf[r] = (uint8_t)mrand48();
		crc16sum = crc16_update(crc16sum, buf[r]);
	}

	MSRT_START(openlog_data_block);
	MSRT_DATA(openlog_data_block, opt.o_size_data);

	transfered = write(fd, buf, opt.o_size_data);
	if (transfered < 0 || transfered != opt.o_size_data) {
		fprintf(stderr, "[fd = %d, rc = %d, %s] write failed\n",
			fd, errno, strerror(errno));
		rc = -errno;
		goto cleanup;
	}

        MSRT_STOP(openlog_data_block);
	fprintf(stdout, "%02d ", n + 1);
	MSRT_DISPLAY_RESULT(openlog_data_block);
	crc32sum = crc32(crc32sum, (const unsigned char *)buf, transfered);
	fprintf(stdout, "successfully written openlog_data_block of "
		"size %ld bytes (CRC32 0x%08x, CRC16 0x%04x) to '%s'\n\n",
		transfered, crc32sum, crc16sum, opt.o_dev_name);

cleanup:
        if (buf)
		free(buf);
	close(fd);

	return rc;
}

int main(int argc, char *argv[])
{
	int rc;

	rc = parseopts(argc, argv);
	if (rc < 0) {
		fprintf(stderr, "try '%s --help' for more information\n", argv[0]);
		return rc;
	}

	fprintf(stdout,
		"starting openlog benchmark with settings\n"
		"baud speed (bps)               : %s\n"
		"number of data blocks          : %d\n"
		"size (bytes) of data blocks    : %d\n"
		"estimated rate (kbytes / secs) : %.2f\n"
		"estimated time (secs) per block: %.2f\n\n",
		BPS_STR(opt.o_dev_speed),
		opt.o_num_data,
		opt.o_size_data,
		(BPS_NUM(opt.o_dev_speed)) / (8 * 1000.0),
		(opt.o_size_data) / ((BPS_NUM(opt.o_dev_speed)) / 8.0));

	for (uint16_t n = 0; n < opt.o_num_data; n++) {
		rc = write_data(n);
		if (rc)
			return rc;
	}

	return rc;
}
