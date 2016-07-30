#include <stdio.h>
#include <stdlib.h>
#include <mpsse.h>

struct mpsse_context *ftdi = NULL;
char address = 0x60;

char read_register(char reg)
{
	char *data = NULL;
	char rv;
	char addr_reading = address | 1;

	Start(ftdi);
	Write(ftdi, &address, 1);
	Write(ftdi, &reg, 1);

	if(GetAck(ftdi) == ACK) {
		Start(ftdi);
		Write(ftdi, &addr_reading, 1);

		if(GetAck(ftdi) == ACK) {
			data = Read(ftdi, 1);
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

void write_register(char reg, char data)
{
	Start(ftdi);
	Write(ftdi, &address, 1);
	Write(ftdi, &reg, 1);
	Write(ftdi, &data, 1);

	Stop(ftdi);
}

int main(void)
{
	int retval = EXIT_FAILURE;

	if((ftdi = MPSSE(I2C, FOUR_HUNDRED_KHZ, MSB)) != NULL && ftdi->open)
	{
		printf("%s initialized at %dHz (I2C)\n", GetDescription(ftdi), GetClock(ftdi));
		char chip_id = read_register(0);
		if (chip_id != 2) {
			fprintf(stderr, "Unknown chip: %x\n", chip_id);
			goto out;
		} else {
			printf("OZ890 rev C detected.\n");
			char softsleep = read_register(0x14);
			char shutdown = read_register(0x15);
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
			char check_yes = read_register(0x1c);
		}
	} else {
		fprintf(stderr, "Failed to initialize MPSSE: %s\n", ErrorString(ftdi));
	}

out:
	Close(ftdi);

	return retval;
}
