#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <mpsse.h>
#include <assert.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <error.h>
#include <errno.h>
#include <endian.h>

#define PWD_FAIL	(1 << 7)
#define PWD_OK		(1 << 6)
#define PWD_BUSY	(1 << 5)
#define ATH_DSG_FAIL	(1 << 3)
#define ATH_DSG_OK	(1 << 2)
#define ATH_CHG_FAIL	(1 << 1)
#define ATH_CHG_OK	(1 << 0)

#define UT_DSBL		(1 << 5)
#define OT_DSBL		(1 << 4)
#define SC_DSBL		(1 << 3)
#define OC_DSBL		(1 << 2)
#define UV_DSBL		(1 << 1)
#define OV_DSBL		(1 << 0)

#define IDL_BLD_ENB	(1 << 6)

struct mpsse_context *ftdi = NULL;
uint8_t address = 0x60;

int debug_level = 0;
char *eeprom_in = NULL;

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
	if (debug_level >= 2) {
		printf("Writing 0x%x to register 0x%x\n", data, reg);
	}
	Start(ftdi);
	Write(ftdi, (char *)&address, 1);
	Write(ftdi, (char *)&reg, 1);
	Write(ftdi, (char *)&data, 1);

	Stop(ftdi);
}

bool is_eeprom_busy(void)
{
	uint8_t byte = read_register(0x5f); // EEPROM Control Register
	return (byte & (1 << 7)); // bit 7 = busy flag
}

void eeprom_lock(void)
{
	while (is_eeprom_busy());
	write_register(0x5f, 0x00);
}

void read_eeprom_word(uint8_t address, uint8_t *buf)
{
	if (eeprom_in) {
		// file
		FILE *f = fopen(eeprom_in, "r");
		if (!f) {
			error(1, errno, "Couldn't open %s", eeprom_in);
		}
		if (fseek(f, address, SEEK_SET)) {
			error(1, errno, "Couldn't seek %s to 0x%x", eeprom_in, address);
		}
		if (fread(buf, 1, 2, f) < 2) {
			error(1, errno, "Couldn't read %s at 0x%x", eeprom_in, address);
		}
	} else {
		// device
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

		eeprom_lock();
	}
}

const unsigned eeprom_size = 128;
uint8_t *read_eeprom(void)
{
	uint8_t *rv = malloc(eeprom_size);
	for (unsigned i = 0; i < eeprom_size; i += 2) {
		read_eeprom_word(i, rv + i);
	}
	return rv;
}

void write_eeprom_word(uint8_t address, uint8_t *buf)
{
	while (is_eeprom_busy());
	write_register(0x5f, 0x52);

	while (is_eeprom_busy()); // wait for eeprom not be busy
	write_register(0x5e, address); // set eeprom address

	while (is_eeprom_busy());
	write_register(0x5d, buf[1]);
	while (is_eeprom_busy());
	write_register(0x5c, buf[0]);

	eeprom_lock();
}

void print_auth_status(uint8_t auth_status)
{
	if (auth_status & PWD_FAIL) {
		puts("Password verification failure");
	}
	if (auth_status & PWD_OK) {
		puts("Password verification success");
	}
	if (auth_status & PWD_BUSY) {
		puts("Password verification busy");
	}
	if (auth_status & ATH_DSG_FAIL) {
		puts("Discharge Authentication failure");
	}
	if (auth_status & ATH_DSG_OK) {
		puts("Discharge Authentication success");
	}
	if (auth_status & ATH_CHG_FAIL) {
		puts("Charge Authentication failure");
	}
	if (auth_status & ATH_CHG_OK) {
		puts("Charge Authentication success");
	}
}

void read_eeprom_file(char *filename, uint8_t *buf)
{
	FILE *f = fopen(filename, "r");
	if (!f) {
		error(1, errno, "Couldn't open %s", filename);
	}
	if (fseek(f, 0, SEEK_END)) {
		error(1, errno, "Couldn't seek %s", filename);
	}
	if (eeprom_size != ftell(f)) {	// a little sanity check
		error(1, 0, "%s is not 128 bytes long", filename);
	}
	rewind(f);
	if (fread(buf, 1, eeprom_size, f) < eeprom_size) {
		error(1, errno, "Couldn't read %d bytes from %s", eeprom_size, filename);
	}
	fclose(f);
}

void write_eeprom_file(char *filename, const uint8_t *buf)
{
	FILE *f = fopen(filename, "wb");
	if (!f) {
		error(1, errno, "Couldn't open %s", filename);
	}
	if (fwrite(buf, 1, eeprom_size, f) < eeprom_size) {
		error(1, errno, "Couldn't write %d bytes to %s", eeprom_size, filename);
	}
	fclose(f);
}

void write_eeprom(char *filename)
{
	uint8_t contents[eeprom_size];
	read_eeprom_file(filename, contents);

	uint8_t password[2];
	// get password
	read_eeprom_word(0x7a, password);
	// grab eeprom
	while (is_eeprom_busy());
	write_register(0x5f, 0x50);
	// enter password
	write_register(0x69, password[0]);
	write_register(0x6a, password[1]);
	// check if it's correct
	uint8_t auth_status = read_register(0x6f);
	// let eeprom go
	eeprom_lock();

	if (debug_level >= 2) {
		print_auth_status(auth_status);
	}
	if (!(auth_status & PWD_OK)) {
		error(1, 0, "Authentication failed");
	}
	// auth success
	// erase the eeprom
	while (is_eeprom_busy());
	write_register(0x5f, 0x53);
	// write eeprom back
	for (unsigned i = 0; i < eeprom_size; i += 2) {
		if (debug_level >= 1) {
			printf("Writing 0x%02x%02x to 0x%x EEPROM address\n", contents[i], contents[i + 1], i);
		}
		write_eeprom_word(i, contents + i);
	}
}

double adc2mv(int16_t sample)
{
	return 1.22 * sample;
}

int16_t v2adc(double voltage)
{
	return voltage / 1.22e-3;
}

unsigned read_cell_voltage(unsigned cell)
{
	assert(cell < 13);
	uint8_t lo = read_register(0x32 + cell * 2);
	uint8_t hi = read_register(0x33 + cell * 2);
	unsigned rv = ((unsigned)hi << 5) + (lo >> 3);
	return rv;
}

// in 100s of µOhms
uint8_t read_sense_resistor(void)
{
	uint8_t tmp[2];
	read_eeprom_word(0x34, tmp);
	return tmp[0] ? tmp[0] : 25;
}

double read_current(void)
{
	uint8_t lo = read_register(0x54);
	uint8_t hi = read_register(0x55);
	int16_t voltage_raw = ((unsigned)hi << 8) + lo;	// in 7.63µVs
	double voltage_V = voltage_raw * 7.63 / 1000000;
	double sense_Ohm = read_sense_resistor() / 10000.0;
	return voltage_V / sense_Ohm;
}

void print_help(char *name)
{
	fprintf(stderr, "Usage: %s [-c] [-d] [-e file] [-F] [-f] [-h] [-o file] [-R mOhm] [-r] [-V ovt,ovr,uvt,uvr] [-v] [-w file]\n\n"
		"Options:\n"
		"	-c			display current\n"
		"	-d			debug output; use multiple times to increase verbosity\n"
		"	-e file			work on the eeprom dump instead of a real device\n"
		"	-F			force operating on an unknown device\n"
		"	-f			display and fix flags\n"
		"	-h			display this help\n"
		"	-o file			read the eeprom to the file\n"
		"	-R mOhm			set the sense resistor resistance\n"
		"	-r			reboot oz890\n"
		"	-v			display voltages\n"
		"	-V ovt,ovr,uvt,uvr	set overvoltage/undervoltage threshold/release values\n"
		"				example: -V 4.2,4.2,2.8,2.9\n"
		"	-w file			write the file into the eeprom\n",
		name);
}

int main(int argc, char *argv[])
{
	int retval = EXIT_SUCCESS;

	int opt;

	char *eeprom_out = NULL;
	char *eeprom_file = NULL;
	bool read_current_ = false;
	bool read_flags = false;
	bool read_voltages = false;
	bool reboot = false;
	bool force = false;
	bool edit_eeprom_file = false;
	bool set_voltage_limits = false;
	bool set_resistance = false;
	double ovt, ovr, uvt, uvr, srr;

	while ((opt = getopt(argc, argv, "cde:Ffho:R:rV:vw:")) != -1) {
		switch (opt) {
		case 'c':
			read_current_ = true;
			break;
		case 'd':
			debug_level++;
			break;
		case 'e':
			eeprom_in = strdup(optarg);
			break;
		case 'F':
			force = true;
			break;
		case 'f':
			read_flags = true;
			break;
		case 'h':
			print_help(argv[0]);
			return EXIT_FAILURE;
		case 'o':
			eeprom_out = strdup(optarg);
			break;
		case 'R':
			edit_eeprom_file = true;
			set_resistance = true;
			sscanf(optarg, "%lf", &srr);
			break;
		case 'r':
			reboot = true;
			break;
		case 'v':
			read_voltages = true;
			break;
		case 'V':
			edit_eeprom_file = true;
			set_voltage_limits = true;
			sscanf(optarg, "%lf,%lf,%lf,%lf", &ovt, &ovr, &uvt, &uvr);
			break;
		case 'w':
			eeprom_file = strdup(optarg);
			break;
		default:
			print_help(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!eeprom_in) {
		if((ftdi = MPSSE(I2C, FOUR_HUNDRED_KHZ, MSB)) != NULL && ftdi->open)
		{
			if (debug_level >= 1)
				printf("%s initialized at %dHz (I2C)\n", GetDescription(ftdi), GetClock(ftdi));
			uint8_t chip_id = read_register(0);
			if (chip_id != 2) {
				fprintf(stderr, "Unknown chip: %x\n", chip_id);
				if (!force) goto out;
			} else {
				printf("OZ890 rev C detected.\n");
			}
			uint8_t tmp[2];
			printf("Factory Name: ");
			for (unsigned i = 0; i < 10; i += 2) {
				read_eeprom_word(0x36 + i, tmp);
				putchar(tmp[0]);
				putchar(tmp[1]);
			}
			putchar('\n');
			printf("Project Name: ");
			read_eeprom_word(0x40, tmp);
			putchar(tmp[0]);
			putchar(tmp[1]);
			read_eeprom_word(0x42, tmp);
			putchar(tmp[0]);
			putchar(tmp[1]);
			read_eeprom_word(0x44, tmp);
			putchar(tmp[0]);
			putchar('\n');
			printf("Version: %d\n", tmp[1]);
		} else {
			fprintf(stderr, "Failed to initialize MPSSE: %s\n", ErrorString(ftdi));
			return -1;
		}
	}

	if (reboot) {
		write_register(0x15, 1);
		sleep(1);
		write_register(0x15, 0);
	}
	if (eeprom_file) {
		write_eeprom(eeprom_file);
	}
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
		if (shutdown & 0x1)
			puts("Shut down by a software request.");
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
		uint8_t fet_disable = read_register(0x1f);
		if (fet_disable & UT_DSBL)
			puts("FET disabled due to Under Temperature");
		if (fet_disable & OT_DSBL)
			puts("FET disabled due to Over Temperature");
		if (fet_disable & SC_DSBL)
			puts("FET disabled due to Short Circuit");
		if (fet_disable & OC_DSBL)
			puts("FET disabled due to Over Current");
		if (fet_disable & UV_DSBL)
			puts("FET disabled due to Under Voltage");
		if (fet_disable & OV_DSBL)
			puts("FET disabled due to Over Voltage");
		uint8_t auth_status = read_register(0x6f);
		print_auth_status(auth_status);
	}
	if (read_voltages) {
		if (!eeprom_in) {
			uint8_t tmp[2];
			read_eeprom_word(0x26, tmp);
			unsigned cells = tmp[0] & 0xf;
			for (unsigned cell = 0; cell < cells; ++cell) {
				unsigned voltage = read_cell_voltage(cell);
				double mv = adc2mv(voltage);
				static double total_v = 0;
				total_v += mv / 1000;
				printf("Cell %2d: %7.2lfmV, total: %5.2lfV\n", cell, mv, total_v);
			}
		}
		uint8_t tmp[2];
		read_eeprom_word(0x2c, tmp);
		printf("Idle bleeding is %s.\n", (tmp[1] & IDL_BLD_ENB) ? "enabled" : "disabled");

		read_eeprom_word(0x48, tmp);
		uint16_t bsv = adc2mv((tmp[0] >> 3) | (tmp[1] << 5));
		printf("Bleeding start voltage: %umV\n", bsv);

		read_eeprom_word(0x4a, tmp);
		uint16_t ovt = adc2mv((tmp[0] >> 3) | (tmp[1] << 5));
		printf("OV Threshold: %umV\n", ovt);

		read_eeprom_word(0x4c, tmp);
		uint16_t ovr = adc2mv((tmp[0] >> 3) | (tmp[1] << 5));
		printf("OV Release: %umV\n", ovr);

		read_eeprom_word(0x4e, tmp);
		uint16_t uvt = adc2mv((tmp[0] >> 3) | (tmp[1] << 5));
		printf("UV Threshold: %umV\n", uvt);

		read_eeprom_word(0x50, tmp);
		uint16_t uvr = adc2mv((tmp[0] >> 3) | (tmp[1] << 5));
		printf("UV Release: %umV\n", uvr);
	}
	if (read_current_) {
		printf("Current: %lfA\n", read_current());
		double sense_Ohm = read_sense_resistor() / 10000.0;
		uint8_t tmp[2];
		read_eeprom_word(0x28, tmp);
		double charge_state_current = 7.63e-6 / sense_Ohm;
		switch (tmp[1] >> 6) {
		case 0:
			charge_state_current *= 12;
			break;
		case 1:
			charge_state_current *= 24;
			break;
		case 2:
			charge_state_current *= 48;
			break;
		case 3:
			charge_state_current *= 120;
			break;
		}
		printf("Charge state current: %lfA\n", charge_state_current);
		double max_charge_current = tmp[0] & 0x1f;
		double max_discharge_current = tmp[1] & 0x3f;
		read_eeprom_word(0x04, tmp);
		int8_t doco = tmp[0] & 0xf0;
		max_discharge_current = (max_discharge_current + doco / 0x10) * 5e-3 / sense_Ohm;
		double max_max_discharge_current = (0x3f + doco / 0x10) * 5e-3 / sense_Ohm;
		printf("Maximum discharge current: %lfA (can be increased to %lfA)\n", max_discharge_current, max_max_discharge_current);
		read_eeprom_word(0x02, tmp);
		int8_t coco = tmp[1] & 0xf0;
		max_charge_current = (max_charge_current + coco / 0x10 - 4) * 5e-3 / sense_Ohm;
		double max_max_charge_current = (0x1f + coco / 0x10 - 4) * 5e-3 / sense_Ohm;
		printf("Maximum charge current: %lfA (can be increased to %lfA)\n", max_charge_current, max_max_charge_current);
		printf("Current sense resistor: %lfmOhm\n", sense_Ohm * 1000);
	}
	if (edit_eeprom_file) {
		if (!eeprom_in) {
			error(1, 0, "Can only edit EEPROM files supplied via `-e filename`");
		}
		uint8_t eeprom_in_buf[eeprom_size];
		read_eeprom_file(eeprom_in, eeprom_in_buf);
		if (set_voltage_limits) {
			*(uint16_t *)(eeprom_in_buf + 0x4a) = htole16(v2adc(ovt) << 3);
			*(uint16_t *)(eeprom_in_buf + 0x4c) = htole16(v2adc(ovr) << 3);
			*(uint16_t *)(eeprom_in_buf + 0x4e) = htole16(v2adc(uvt) << 3);
			*(uint16_t *)(eeprom_in_buf + 0x50) = htole16(v2adc(uvr) << 3);
		}
		if (set_resistance) {
			if (srr < 0.1 || srr > 25.5) {
				error(1, 0, "Sense resistor resistance must be between 0.1 and 25.5mOhm");
			}
			uint8_t srr_converted = (uint8_t)(srr * 10);
			printf("Setting sense resistor resistance to %lfmOhm\n", 0.1 * srr_converted);
			*(uint8_t *)(eeprom_in_buf + 0x34) = srr_converted;
		}
		write_eeprom_file(eeprom_in, eeprom_in_buf);
	}
	if (eeprom_out) {
		FILE *f = fopen(eeprom_out, "wb");
		uint8_t *buf = read_eeprom();
		fwrite(buf, eeprom_size, 1, f);
		fclose(f);
		free(buf);
	}

out:
	Close(ftdi);

	return retval;
}
