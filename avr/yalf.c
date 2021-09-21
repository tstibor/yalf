#ifndef F_CPU
#warning "F_CPU not defined, defining 16000000 (16Mhz)"
#define F_CPU 16000000UL
#endif

#ifndef BAUD
#warning "BAUD not defined, defining 230400 (230Kbit/sec)"
#define BAUD 230400UL
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/setbaud.h>
#include <util/delay.h>
#include <util/atomic.h>
#include "diskio.h"
#include "ff.h"

#define USART_CONFIG_FILENAME "config.txt"

/* Stored filenames have form: LOGXXXXX.BFL,
   where XXXXX is non-negative integer. */
#define FILENAME_PREFIX	"LOG"
#define FILENAME_SUFFIX	".BFL"

/* Blinking LED error codes. */
#define ERR_BLINK_SD_INIT	2
#define ERR_BLINK_SD_DIR	3
#define ERR_BLINK_SD_OPEN	4
#define ERR_BLINK_SD_WRITE	5
#define ERR_BLINK_SD_READ	6
#define ERR_BLINK_SD_CLOSE	7
#define ERR_BLINK_SD_SYNC       8
#define ERR_BLINK_USART_CONFIG  9

/* Pin of status LED. */
#define LED_STATUS_ON()         (PORTB |=  (1 << PB5))
#define LED_STATUS_OFF()        (PORTB &= ~(1 << PB5))
#define LED_STATUS_TOGGLE()	(PORTB ^=  (1 << PB5))

/* Pin of SPI/SDCard LED. */
#define LED_SPI_SDCARD_ON()	(PORTD |=  (1 << PD5))
#define LED_SPI_SDCARD_OFF()	(PORTD &= ~(1 << PD5))
#define LED_SPI_SDCARD_TOGGLE()	(PORTD ^=  (1 << PD5))

#define BUF_USART_SIZE	256
#define BUF_SDCARD_SIZE 128

/* Timer variable incremented each 10 millisecond. */
volatile uint32_t timer_10msec_cnt = 0;

/* USART ringbuffer. */
volatile uint8_t buf_usart_head = 0;
volatile uint8_t buf_usart_tail = 0;
uint8_t buf_usart[BUF_USART_SIZE] = {0};

struct usart_config {
	uint32_t baud_rate;
	uint8_t data_bits;
	char parity;
	uint8_t stop_bit;
	uint8_t timeout_sec_close;
};

ISR(TIMER0_COMPA_vect)
{
	/* Timer interrupt, for calling disk_timerproc() in
	   fatfs library each 100Hz (10ms) . */
        disk_timerproc();
	timer_10msec_cnt++;
}

void error_blink_halt(uint8_t code)
{
        while (1) {
                for (uint8_t c = 0; c < code; c++) {
			LED_STATUS_ON();
			_delay_ms(200);
			LED_STATUS_OFF();
			_delay_ms(200);
                }
                _delay_ms(2000);
        }
}

void timer_init(void)
{
	/* Initialization for TIMER0_COMPA_vect. */
	TCCR0A |= (1 << WGM01);

	/* Set prescaler to 1024 (0b101). */
	TCCR0B |= (1 << CS02) | (1 << CS00);

	/* We want an interrupt each 10ms (1 * 10^(-2)),
	   run at 16Mhz (16 * 10^6) and use a prescaler of 1024, thus:
	   Timer count = Delay / Clock time period - 1
	   155.25 = (1 * 10^(-2)) / (1 / (16 * 10^6 / 1024)) - 1
	   If this value is reached, then 10ms is passed. */
	OCR0A = F_CPU / 1024 / 100 - 1;

	/* Enable compare and match ISR(TIMER0_COMPA_vect). */
	TIMSK0 |= (1 << OCIE0A);
}

void led_init(void)
{
	/* Status LED. */
	DDRB |= (1 << PB5);

        /* SPI/SDCard status LED. */
	DDRD |= (1 << PD5);
}

void usart_init(void)
{
	UBRR0H = UBRRH_VALUE;
	UBRR0L = UBRRL_VALUE;

#if USE_2X
	UCSR0A |= (1 << U2X0);
#else
	UCSR0A &= ~(1 << U2X0);
#endif

	UCSR0C = (3 << UCSZ00);
#if 0
	/* Asynchronous 8N1 */
	UCSR0C |= (0 << UPM01)   | (0 << UPM00)   /* No parity. */
		| (1 << UCSZ01)  | (1 << UCSZ00)  /* Eight bits of data. */
		| (0 << UMSEL01) | (0 << UMSEL00) /* Asynchronous USART. */
		| (0 << USBS0);			  /* One stop bit. */
#endif
	/* Enable USART TX, RX and RX interrupt service routine. */
        UCSR0B |= (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
}

void usart_tx_c(uint8_t d)
{
	loop_until_bit_is_set(UCSR0A, UDRE0);
	UDR0 = d;
}

void usart_tx_s(const char *s)
{
	while (*s != '\0')
		usart_tx_c(*s++);
	usart_tx_c('\r');
	usart_tx_c('\n');
}

void usart_tx_s_int(const char *s, uint32_t n)
{
	char _s[32] = {0};

	snprintf(_s, sizeof(_s), "%s %lu", s, n);
	usart_tx_s(_s);
}

void usart_tx_crc16(uint16_t n)
{
	char _s[16] = {0};

	snprintf(_s, sizeof(_s), "CRC16 0x%04x", n);
	usart_tx_s(_s);
}

ISR(USART_RX_vect)
{
	if (!(UCSR0A & (1 << RXC0))) {
		UDR0;
                return;
	}

	uint8_t next_tail = (uint8_t)(buf_usart_tail + 1) % BUF_USART_SIZE;
	uint8_t d = UDR0;

	if (next_tail == buf_usart_head)
		return;

	buf_usart[buf_usart_tail] = d;
	buf_usart_tail = next_tail;
}

void filename_number_last(uint32_t *file_number_last, bool *file_config_found)
{
	FRESULT fr;
	DIR dir;
	FILINFO file_info;
	int rc, n, file_num_last = 0;
	bool file_conf_found = false;

	memset(&dir, 0, sizeof(DIR));
	fr = f_opendir(&dir, "/");
	if (fr)
		error_blink_halt(ERR_BLINK_SD_DIR);

	memset(&file_info, 0, sizeof(file_info));
	while (1) {
		fr = f_readdir(&dir, &file_info);
		if (fr != FR_OK || !file_info.fname[0])
			break;

		if (!strncmp(USART_CONFIG_FILENAME, file_info.fname, strlen(USART_CONFIG_FILENAME)))
			file_conf_found = true;

		if (strncmp(FILENAME_PREFIX, file_info.fname, 3))
			continue;

		/* %*[^0123456789] ignore input until a digit is found. */
                rc = sscanf(file_info.fname, "%*[^0123456789]%d%*[^0123456789]", &n);
		if (rc != 1)
			continue;

		if (file_num_last < n)
			file_num_last = n;
	}

	fr = f_closedir(&dir);
	if (fr)
		error_blink_halt(ERR_BLINK_SD_DIR);

	*file_number_last = file_num_last;
	*file_config_found = file_conf_found;
}

int parse_usart_config(struct usart_config *usart_config, const char *usart_config_str)
{
	return 0;
}

int main()
{
	FATFS	 fatfs;
	FIL	 fil;
	FRESULT	 fr;
	DSTATUS	 ds;
	UINT	 bw, br;
	uint32_t filename_number = 0;
	char	 filename[13] = {0};
	bool     file_config_found;
	char     usart_config_str[128] = {0};
	int      rc;
	struct usart_config usart_config = {
		.baud_rate = 234000,
		.data_bits = 8,
		.parity = 'N',
		.stop_bit = 1,
		.timeout_sec_close = 3
	};

        timer_init();
	led_init();
	usart_init();
	sei();

	LED_STATUS_OFF();
	LED_SPI_SDCARD_OFF();

	/* STA_NOINIT   0x01    Drive not initialized.
           STA_NODISK   0x02    No medium in the drive.
           STA_PROTECT  0x04    Write protected. */
        ds = disk_initialize(0);
        if (ds)
                error_blink_halt(ERR_BLINK_SD_INIT);

        fr = f_mount(&fatfs, "/", 0);
        if (fr != FR_OK)
                error_blink_halt(ERR_BLINK_SD_INIT);

	filename_number_last(&filename_number, &file_config_found);

	fr = f_open(&fil, USART_CONFIG_FILENAME,
		    !file_config_found ?
		    FA_WRITE | FA_CREATE_NEW :
		    FA_OPEN_EXISTING | FA_READ);
	if (fr != FR_OK)
		error_blink_halt(ERR_BLINK_SD_OPEN);

	if (!file_config_found) {
		/* BAUD,databits,parity,stopbit,close timeout secs */
		snprintf(usart_config_str, sizeof(usart_config_str), "%lu,%hhu,%c,%hhu,%hhu",
			 usart_config.baud_rate, usart_config.data_bits,
			 usart_config.parity, usart_config.stop_bit, usart_config.timeout_sec_close);
		fr = f_write(&fil, usart_config_str, strlen(usart_config_str), &bw);
		if (!(fr == FR_OK && bw == strlen(usart_config_str)))
			error_blink_halt(ERR_BLINK_SD_WRITE);
	} else {
		fr = f_read(&fil, usart_config_str, sizeof(usart_config_str), &br);
		if (fr != FR_OK)
			error_blink_halt(ERR_BLINK_SD_READ);

		rc = sscanf(usart_config_str, "%lu,%hhu,%c,%hhu,%hhu",
			    &usart_config.baud_rate, &usart_config.data_bits,
			    &usart_config.parity, &usart_config.stop_bit, &usart_config.timeout_sec_close);
		if (rc != 5)
			error_blink_halt(ERR_BLINK_USART_CONFIG);
	}

	fr = f_close(&fil);
	if (fr != FR_OK)
		error_blink_halt(ERR_BLINK_SD_CLOSE);

new_file:
	filename_number++;
	memset(filename, 0, sizeof(filename));
        snprintf(filename, sizeof(filename), "%s%05lu%s",
                 FILENAME_PREFIX, filename_number, FILENAME_SUFFIX);

        fr = f_open(&fil, filename, FA_WRITE | FA_CREATE_NEW);
        if (fr != FR_OK)
                error_blink_halt(ERR_BLINK_SD_OPEN);

	uint8_t buf_sdcard[BUF_SDCARD_SIZE] = {0};
	uint8_t uart_d;
	uint8_t i = 0;
	uint32_t timer_last_write = 0;
	uint32_t written_total = 0;

	usart_tx_s(filename);

	while (1) {

		while (i < BUF_SDCARD_SIZE && (buf_usart_head != buf_usart_tail)) {
			ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
				uart_d = buf_usart[buf_usart_head];
				buf_usart_head = (uint8_t)(buf_usart_head + 1) % BUF_USART_SIZE;
			}
			buf_sdcard[i++] = uart_d;
		}

		if (i > 0) {
			LED_SPI_SDCARD_ON();
			bw = 0;
			fr = f_write(&fil, buf_sdcard, i, &bw);
			if (fr == FR_OK && bw == i) {
				i = 0;
				written_total += bw;
				timer_last_write = timer_10msec_cnt;
			} else
				error_blink_halt(ERR_BLINK_SD_WRITE);
			LED_SPI_SDCARD_OFF();
		}

		/* 3 seconds passed after last write, close file and create a new one. */
		if (timer_last_write > 0 && timer_10msec_cnt - timer_last_write > 300) {
			fr = f_close(&fil);
			if (fr != FR_OK)
				error_blink_halt(ERR_BLINK_SD_CLOSE);
			usart_tx_s_int("written", written_total);
			goto new_file;
		}
	} /* End-while */

	/* We will never be here. */
	cli();

	return 0;
}
