#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <mpsse.h>
#include <assert.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>

struct mpsse_context *ftdi = NULL;
uint8_t address = 0x60;

uint8_t read_register(uint8_t reg)
{
	uint8_t *data = NULL;
	uint8_t rv;
	uint8_t addr_reading = address | 1;

	Start(ftdi);
	Write(ftdi, (char *)&address, 1);
	Write(ftdi, (char *)&reg, 1);

	if(GetAck(ftdi) == ACK) {
		Start(ftdi);
		Write(ftdi, (char *)&addr_reading, 1);

		if(GetAck(ftdi) == ACK) {
			data = (uint8_t *)Read(ftdi, 1);
			if(data) {
				printf("got %x\n", *(data));
				rv = *data;
				free(data);
			}
			SendNacks(ftdi);
			/* Read in one dummy byte, with a NACK */
			Read(ftdi, 1);
			}
	}

	Stop(ftdi);

	return rv;
}

void write_register(uint8_t reg, uint8_t data)
{
	Start(ftdi);
	Write(ftdi, (char *)&address, 1);
	Write(ftdi, (char *)&reg, 1);
	Write(ftdi, (char *)&data, 1);

	Stop(ftdi);
}


unsigned read_cell_voltage(unsigned cell)
{
	assert(cell < 13);
	uint8_t lo = read_register(0x32 + cell * 2);
	uint8_t hi = read_register(0x33 + cell * 2);
	unsigned rv = ((unsigned)hi << 5) + (lo >> 3);
	printf("cell %d: %d %d %lfmV\n", cell, hi, lo, 1.22 * rv);
	return rv;
}

unsigned read_current(void)
{
	uint8_t lo = read_register(0x54);
	uint8_t hi = read_register(0x55);
	return ((unsigned)hi << 8) + lo;
}

bool is_eeprom_busy(void)
{
	uint8_t byte = read_register(0x5f); // EEPROM Control Register
	return (byte & (1 << 7)); // bit 7 = busy flag
}

void read_eeprom_word(uint8_t address, uint8_t *buf)
{
	while (is_eeprom_busy()); // wait for eeprom not be busy
	write_register(0x5e, address); // set eeprom address to read

	while (is_eeprom_busy());
	write_register(0x5f, 0x55); // b01010101 (or 0x55) set eeprom access & word reading mode

	while (is_eeprom_busy());
	buf[1] = read_register(0x5d); // odd addr
	while (is_eeprom_busy());
	buf[0] = read_register(0x5c); // even addr

	while (is_eeprom_busy());
	write_register(0x5f, 0x00); // disable eeprom access
}

const unsigned eeprom_size = 128;
uint8_t *read_eeprom(void)
{
	uint8_t *rv = malloc(eeprom_size);
	for (unsigned i = 0; i < 128; i += 2) {
		read_eeprom_word(i, rv + i);
	}
	return rv;
}


void print_help(char *name)
{
	fprintf(stderr, "Usage: %s [-c] [-f] [-h] [-o file] [-v]\n\n"
		"Options:\n"
		"	-c		display current\n"
		"	-f		display and fix flags\n"
		"	-h		display this help\n"
		"	-o file		read the eeprom to the file\n"
		"	-v		display voltages\n",
		name);
}

int main(int argc, char *argv[])
{
	int retval = EXIT_FAILURE;

	int opt;

	char *eeprom_out = NULL;
	bool read_current_ = false;
	bool read_flags = false;
	bool read_voltages = false;
	int debug_level = 0;

	while ((opt = getopt(argc, argv, "cho:v")) != -1) {
		switch (opt) {
		case 'c':
			read_current_ = true;
			break;
		case 'f':
			read_flags = true;
		case 'h':
			print_help(argv[0]);
			return retval;
		case 'o':
			eeprom_out = strdup(optarg);
			break;
		case 'v':
			read_voltages = true;
			break;
		default:
			print_help(argv[0]);
			return retval;
		}
	}

	if((ftdi = MPSSE(I2C, FOUR_HUNDRED_KHZ, MSB)) != NULL && ftdi->open)
	{
		printf("%s initialized at %dHz (I2C)\n", GetDescription(ftdi), GetClock(ftdi));
		uint8_t chip_id = read_register(0);
		if (chip_id != 2) {
			fprintf(stderr, "Unknown chip: %x\n", chip_id);
			goto out;
		} else {
			printf("OZ890 rev C detected.\n");
			if (read_flags) {
				uint8_t softsleep = read_register(0x14);
				uint8_t shutdown = read_register(0x15);
				if (shutdown & 0x10) {
					printf("Battery is unbalanced (permanent failure flag). Clearing...\n");
					write_register(0x15, 0x10);
				}
				if (shutdown & 0x8)
					printf("MOSFET failure detected.\n");
				if (shutdown & 0x4)
					printf("Voltage High Permanent Failure.\n");
				if (shutdown & 0x2)
					printf("Voltage Low Permanent Failure.\n");
				uint8_t check_yes = read_register(0x1c);
			}
			if (read_voltages) {
				for (int cell = 0; cell < 13; ++cell) {
					read_cell_voltage(cell);
				}
			}
			if (read_current_) {
				printf("Current: %u", read_current());
			}
			if (eeprom_out) {
				FILE *f = fopen(eeprom_out, "wb");
				uint8_t *buf = read_eeprom();
				fwrite(buf, eeprom_size, 1, f);
				fclose(f);
				free(buf);
			}
		}
	} else {
		fprintf(stderr, "Failed to initialize MPSSE: %s\n", ErrorString(ftdi));
	}

out:
	Close(ftdi);

	return retval;
}
