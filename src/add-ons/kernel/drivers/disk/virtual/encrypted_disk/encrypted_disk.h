/*
 * Copyright 2025, Haiku Project. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Kyle Ambroff-Kao, kyle@ambroffkao.com
 */
#ifndef ENCRYPTED_DISK_H
#define ENCRYPTED_DISK_H


#include <Drivers.h>


// ioctl commands for encrypted disk control
enum {
	ENCRYPTED_DISK_IOCTL_SET_PASSPHRASE = B_DEVICE_OP_CODES_END + 0x1000,
	ENCRYPTED_DISK_IOCTL_CLEAR_KEY,
	ENCRYPTED_DISK_IOCTL_GET_STATUS,
	// Control device specific ioctls
	ENCRYPTED_DISK_IOCTL_INIT_PARTITION,
	ENCRYPTED_DISK_IOCTL_UNLOCK_PARTITION,
	ENCRYPTED_DISK_IOCTL_LOCK_PARTITION,
	ENCRYPTED_DISK_IOCTL_STATUS_PARTITION
};


// Maximum passphrase length (reasonable limit for security)
#define ENCRYPTED_DISK_MAX_PASSPHRASE_LENGTH 512

// Status flags
enum { ENCRYPTED_DISK_STATUS_LOCKED = 0, ENCRYPTED_DISK_STATUS_UNLOCKED = 1 };

// Structure for setting passphrase
struct encrypted_disk_ioctl_set_passphrase {
	uint8 passphrase[ENCRYPTED_DISK_MAX_PASSPHRASE_LENGTH];
	uint32 passphrase_length;
	uint32 reserved[4]; // For future use
};

// Structure for getting device status
struct encrypted_disk_ioctl_status {
	uint32 status; // ENCRYPTED_DISK_STATUS_*
	uint64 device_size; // Size of decrypted device
	uint32 cipher_type; // Current cipher type
	uint32 reserved[4]; // For future use
};

// Structure for control device operations on partitions
struct encrypted_disk_ioctl_partition_control {
	char partition_path[256]; // Path to the encrypted partition
	union {
		encrypted_disk_ioctl_set_passphrase passphrase;
		encrypted_disk_ioctl_status status;
	} data;
};


#endif // ENCRYPTED_DISK_H
