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

int debug_level = 0;

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
				rv = *data;
				free(data);
			}
			SendNacks(ftdi);
			/* Read in one dummy byte, with a NACK */
			Read(ftdi, 1);
			}
	}

	Stop(ftdi);
	if (debug_level >= 2) {
		printf("Register 0x%x read 0x%x\n", reg, rv);
	}
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

	if (debug_level >= 1) {
		printf("EEPROM address 0x%x read 0x%02x%02x\n", address, buf[0], buf[1]);
	}

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
	fprintf(stderr, "Usage: %s [-c] [-d] [-f] [-h] [-o file] [-v]\n\n"
		"Options:\n"
		"	-c		display current\n"
		"	-d		debug output; use multiple times to increase verbosity\n"
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

	while ((opt = getopt(argc, argv, "cdfho:v")) != -1) {
		switch (opt) {
		case 'c':
			read_current_ = true;
			break;
		case 'd':
			debug_level++;
			break;
		case 'f':
			read_flags = true;
			break;
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
		if (debug_level >= 1)
			printf("%s initialized at %dHz (I2C)\n", GetDescription(ftdi), GetClock(ftdi));
		uint8_t chip_id = read_register(0);
		if (chip_id != 2) {
			fprintf(stderr, "Unknown chip: %x\n", chip_id);
			goto out;
		} else {
			printf("OZ890 rev C detected.\n");
			if (read_flags) {
				uint8_t tmp[2];
				read_eeprom_word(0x32, tmp);
				bool software_mode = !(tmp[0] & 1);
				if (software_mode)
					puts("Software mode.");
				else {
					printf("Hardware mode. Bleeding is %s.\n",
							(tmp[0] & 2) ? "enabled" : "disabled");
				}
				uint8_t softsleep = read_register(0x14);
				if (softsleep & 2)
					puts("Woken up by short circuit.");
				if (softsleep & 0x10)
					puts("Device is in low power state.");
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
				if (check_yes & 1)
					puts("Undervoltage detected.");
				if (check_yes & 2)
					puts("Cell voltage is extremely low (permanent failure flag)!");
				if (check_yes & 4)
					puts("Cell voltage is extremely high (permanent failure flag)!");
				if (check_yes & 8)
					puts("MOSFET failure (permanent failure flag)!");
				if (check_yes & 0x10)
					puts("Cells are unbalanced (permanent failure flag)!");
				if (check_yes & 0x20)
					puts("Overvoltage detected.");
				if (check_yes & 0x40)
					puts("Temperature is too low.");
				if (check_yes & 0x80)
					puts("Temperature is too high!");
				if (software_mode) {
					uint8_t fet_enable = read_register(0x1e);
					if ((fet_enable & 1) == 0)
						printf("Discharge MOSFET is disabled by software.\n");
					if ((fet_enable & 2) == 0)
						printf("Charge MOSFET is disabled by software.\n");
					if ((fet_enable & 4) == 0)
						printf("Precharge MOSFET is disabled by software.\n");
				}
				uint8_t cd_state = read_register(0x20);
				if (cd_state & 8) {
					puts("Battery is charging.");
				} else {
					if (debug_level) puts("Battery is not charging.");
				}
				if (cd_state & 4) {
					puts("Battery is discharging.");
				} else {
					if (debug_level) puts("Battery is not discharging.");
				}
			}
			if (read_voltages) {
				for (int cell = 0; cell < 13; ++cell) {
					unsigned voltage = read_cell_voltage(cell);
					// FIXME use different coefficients according to the configuration
					printf("Cell %d: %lfmV\n", cell, 1.22 * voltage);
				}
			}
			if (read_current_) {
				printf("Current: %u\n", read_current());
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
