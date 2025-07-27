/*
 * Copyright 2025, Haiku Project. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Control utility for encrypted disk partitions - New implementation
 * Works with the control device at /dev/disk/virtual/encrypted_disk/control
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// Note: PBKDF2 key derivation is done in the kernel driver during unlock

#include <Drivers.h>
#include <SupportDefs.h>

#include "encrypted_disk.h"

// Control device path
static const char* kControlDevicePath = "/dev/disk/virtual/encrypted_disk/control";


static void
usage(const char* program_name)
{
	printf("Usage: %s <command> <partition>\n", program_name);
	printf("\n");
	printf("Commands:\n");
	printf("  init <partition>    Initialize a new encrypted partition\n");
	printf("  unlock <partition>  Unlock an encrypted partition with passphrase\n");
	printf("  lock <partition>    Lock an encrypted partition (clear keys)\n");
	printf("  status <partition>  Show partition encryption status\n");
	printf("\n");
	printf("Examples:\n");
	printf("  %s init /dev/disk/ata/0/slave/0\n", program_name);
	printf("  %s unlock /dev/disk/ata/0/slave/0\n", program_name);
	printf("  %s status /dev/disk/ata/0/slave/0\n", program_name);
	printf("  %s lock /dev/disk/ata/0/slave/0\n", program_name);
	printf("\n");
	printf("Note: The partition must have the 'Haiku data (encrypted)' GPT type\n");
	printf("      for automatic detection during boot.\n");
}


static status_t
read_passphrase(char* passphrase, size_t max_length)
{
	printf("Enter passphrase: ");
	fflush(stdout);

	// Disable echo for secure input
	struct termios old_termios, new_termios;
	tcgetattr(STDIN_FILENO, &old_termios);
	new_termios = old_termios;
	new_termios.c_lflag &= ~ECHO;
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	// Read passphrase
	char* result = fgets(passphrase, max_length, stdin);

	// Restore echo
	tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
	printf("\n");

	if (result == nullptr) {
		printf("Error reading passphrase\n");
		return B_ERROR;
	}

	// Remove trailing newline
	size_t len = strlen(passphrase);
	if (len > 0 && passphrase[len - 1] == '\n') {
		passphrase[len - 1] = '\0';
		len--;
	}

	if (len == 0) {
		printf("Empty passphrase not allowed\n");
		return B_BAD_VALUE;
	}

	return B_OK;
}


static status_t
init_partition(const char* partition_path)
{
	printf("Initializing encrypted partition: %s\n", partition_path);
	printf("WARNING: This will overwrite the first 4096 bytes of the partition!\n");
	printf("Make sure this is the correct partition. Continue? (y/N): ");
	fflush(stdout);

	char response;
	if (scanf(" %c", &response) != 1 || (response != 'y' && response != 'Y')) {
		printf("Aborted.\n");
		return B_CANCELED;
	}

	// Consume the remaining newline character
	int c;
	while ((c = getchar()) != '\n' && c != EOF)
		;

	// Get passphrase from user
	char passphrase[ENCRYPTED_DISK_MAX_PASSPHRASE_LENGTH];
	printf("\nPlease set a passphrase for this encrypted volume.\n");
	status_t result = read_passphrase(passphrase, sizeof(passphrase));
	if (result != B_OK)
		return result;

	// Confirm passphrase
	printf("Confirm passphrase: ");
	fflush(stdout);
	char confirm[ENCRYPTED_DISK_MAX_PASSPHRASE_LENGTH];

	// Disable echo for confirmation
	struct termios old_termios, new_termios;
	tcgetattr(STDIN_FILENO, &old_termios);
	new_termios = old_termios;
	new_termios.c_lflag &= ~ECHO;
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	char* confirm_result = fgets(confirm, sizeof(confirm), stdin);
	tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
	printf("\n");

	if (confirm_result == nullptr) {
		memset(passphrase, 0, sizeof(passphrase));
		printf("Error reading confirmation\n");
		return B_ERROR;
	}

	// Remove trailing newline
	size_t confirm_len = strlen(confirm);
	if (confirm_len > 0 && confirm[confirm_len - 1] == '\n')
		confirm[confirm_len - 1] = '\0';

	if (strcmp(passphrase, confirm) != 0) {
		memset(passphrase, 0, sizeof(passphrase));
		memset(confirm, 0, sizeof(confirm));
		printf("Error: Passphrases do not match\n");
		return B_ERROR;
	}

	memset(confirm, 0, sizeof(confirm));

	// Open control device
	int control_fd = open(kControlDevicePath, O_RDWR);
	if (control_fd < 0) {
		printf("Error: Cannot open control device '%s': %s\n", kControlDevicePath, strerror(errno));
		memset(passphrase, 0, sizeof(passphrase));
		return errno;
	}

	// Prepare ioctl structure
	encrypted_disk_ioctl_partition_control control;
	strlcpy(control.partition_path, partition_path, sizeof(control.partition_path));

	// Copy passphrase data
	size_t passphrase_len = strlen(passphrase);
	memcpy(control.data.passphrase.passphrase, passphrase, passphrase_len);
	control.data.passphrase.passphrase_length = passphrase_len;

	// Clear sensitive data
	memset(passphrase, 0, sizeof(passphrase));

	// Call kernel to initialize the partition
	printf("Creating encrypted disk header and validation data...\n");
	result = ioctl(control_fd, ENCRYPTED_DISK_IOCTL_INIT_PARTITION, &control, sizeof(control));

	// Clear passphrase from control structure
	memset(&control.data.passphrase, 0, sizeof(control.data.passphrase));
	close(control_fd);

	if (result != B_OK) {
		printf("Error: Failed to initialize partition: %s\n", strerror(result));
		return result;
	}

	printf("Encrypted partition initialized successfully!\n");
	printf("The partition is now ready for use with passphrase validation.\n");

	printf("\nIMPORTANT:\n");
	printf("- Make sure the partition type is set to 'Haiku data (encrypted)'\n");
	printf("- Your passphrase is required to access this volume\n");
	printf("- DO NOT FORGET YOUR PASSPHRASE - data cannot be recovered without it\n");

	return B_OK;
}


static status_t
unlock_partition(const char* partition_path)
{
	char passphrase[ENCRYPTED_DISK_MAX_PASSPHRASE_LENGTH];
	status_t result = read_passphrase(passphrase, sizeof(passphrase));
	if (result != B_OK)
		return result;

	printf("Unlocking device...\n");

	// Open control device
	int fd = open(kControlDevicePath, O_RDWR);
	if (fd < 0) {
		printf("Error: Cannot open control device '%s': %s\n", kControlDevicePath, strerror(errno));
		printf("The encrypted_disk driver is not loaded.\n");
		printf("Please check that the driver is available at:\n");
		printf("  /boot/system/add-ons/kernel/drivers/disk/virtual/encrypted_disk\n");
		// Clear passphrase from memory
		memset(passphrase, 0, sizeof(passphrase));
		return errno;
	}

	// Prepare control structure
	encrypted_disk_ioctl_partition_control control;
	memset(&control, 0, sizeof(control));
	strlcpy(control.partition_path, partition_path, sizeof(control.partition_path));

	// Include passphrase in unlock request
	size_t passphrase_len = strlen(passphrase);
	memcpy(control.data.passphrase.passphrase, passphrase, passphrase_len);
	control.data.passphrase.passphrase_length = passphrase_len;

	// Unlock partition with passphrase
	result = ioctl(fd, ENCRYPTED_DISK_IOCTL_UNLOCK_PARTITION, &control, sizeof(control));
	if (result != B_OK) {
		if (result == B_PERMISSION_DENIED) {
			printf("Error: Incorrect passphrase\n");
		} else if (result == B_NOT_INITIALIZED) {
			printf("Error: Partition has no validation data. Use 'encrypted-disk init %s' to add "
				   "validation.\n",
				partition_path);
		} else {
			printf("Error: Failed to unlock partition: %s\n", strerror(result));
		}
		close(fd);
		memset(passphrase, 0, sizeof(passphrase));
		return result;
	}

	close(fd);

	// Clear passphrase from memory
	memset(passphrase, 0, sizeof(passphrase));
	memset(&control.data.passphrase, 0, sizeof(control.data.passphrase));

	printf("Partition unlocked successfully!\n");
	printf("Virtual device created at: /dev/disk/virtual/encrypted_disk/N/raw\n");
	printf("You can now use DriveSetup to partition and format the decrypted device.\n");

	return B_OK;
}


static status_t
lock_partition(const char* partition_path)
{
	// Open control device
	int fd = open(kControlDevicePath, O_RDWR);
	if (fd < 0) {
		printf("Error: Cannot open control device '%s': %s\n", kControlDevicePath, strerror(errno));
		return errno;
	}

	// Prepare control structure
	encrypted_disk_ioctl_partition_control control;
	memset(&control, 0, sizeof(control));
	strlcpy(control.partition_path, partition_path, sizeof(control.partition_path));

	status_t result = ioctl(fd, ENCRYPTED_DISK_IOCTL_LOCK_PARTITION, &control, sizeof(control));
	close(fd);

	if (result != B_OK) {
		printf("Error: Failed to lock partition: %s\n", strerror(result));
		return result;
	}

	printf("Partition locked successfully\n");
	return B_OK;
}


static status_t
show_status(const char* partition_path)
{
	// Open control device
	int fd = open(kControlDevicePath, O_RDWR);
	if (fd < 0) {
		printf("Error: Cannot open control device '%s': %s\n", kControlDevicePath, strerror(errno));
		printf("The encrypted_disk driver is not loaded.\n");
		return errno;
	}

	// Use control device
	encrypted_disk_ioctl_partition_control control;
	memset(&control, 0, sizeof(control));
	strlcpy(control.partition_path, partition_path, sizeof(control.partition_path));

	status_t result = ioctl(fd, ENCRYPTED_DISK_IOCTL_STATUS_PARTITION, &control, sizeof(control));
	close(fd);

	if (result != B_OK) {
		if (result == B_BAD_VALUE) {
			printf("Partition: %s\n", partition_path);
			printf("Status: Not encrypted (no valid header found)\n");
		} else {
			printf("Error: Failed to get partition status: %s\n", strerror(result));
		}
		return result;
	}

	printf("Partition: %s\n", partition_path);
	printf("Status: %s\n",
		(control.data.status.status == ENCRYPTED_DISK_STATUS_UNLOCKED) ? "Unlocked" : "Locked");
	printf("Data size: %" B_PRIu64 " bytes (%.2f MB)\n", control.data.status.device_size,
		(double)control.data.status.device_size / (1024.0 * 1024.0));

	printf("Cipher: ");
	switch (control.data.status.cipher_type) {
		case 1: // CIPHER_AES256_XTS
			printf("AES-256-XTS\n");
			break;
		default:
			printf("Unknown (%u)\n", control.data.status.cipher_type);
			break;
	}

	return B_OK;
}


int
main(int argc, char* argv[])
{
	if (argc != 3) {
		usage(argv[0]);
		return 1;
	}

	const char* command = argv[1];
	const char* partition_path = argv[2];

	// Seed random number generator for salt generation
	srandom(time(NULL) ^ getpid());

	status_t result;

	if (strcmp(command, "init") == 0) {
		result = init_partition(partition_path);
	} else if (strcmp(command, "unlock") == 0) {
		result = unlock_partition(partition_path);
	} else if (strcmp(command, "lock") == 0) {
		result = lock_partition(partition_path);
	} else if (strcmp(command, "status") == 0) {
		result = show_status(partition_path);
	} else {
		printf("Error: Unknown command '%s'\n", command);
		usage(argv[0]);
		return 1;
	}

	return (result == B_OK) ? 0 : 1;
}
