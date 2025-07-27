/*
 * Copyright 2025, Haiku Project. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 *
 * Authors:
 *		Kyle Ambroff-Kao, kyle@ambroffkao.com
 */
#include "encrypted_disk.h"

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <os/drivers/Drivers.h>
#include <os/drivers/KernelExport.h>
#include <os/drivers/device_manager.h>
#include <os/drivers/io_requests.h>

#include <AutoDeleter.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <util/atomic.h>

#include <algorithm>
#include <fs/devfs.h>
#include <heap.h>
#include <lock.h>
#include <vm/vm.h>

#include "aes_xts.hpp"
#include "pbkdf2.hpp"
#include "sha256.hpp"

// PBKDF2 iterations for key derivation
#define PBKDF2_ITERATIONS 100000

// #define TRACE_ENCRYPTED_DISK
#ifdef TRACE_ENCRYPTED_DISK
#define TRACE(x...) dprintf("encrypted_disk: " x)
#else
#define TRACE(x...) ;
#endif
#define TRACE_ALWAYS(x...) dprintf("encrypted_disk: " x)
#define TRACE_ERROR(x...) dprintf("\33[33mencrypted_disk:\33[0m " x)


#define ENCRYPTED_DISK_DRIVER_MODULE_NAME "drivers/disk/virtual/encrypted_disk/driver_v1"
#define ENCRYPTED_DISK_DEVICE_MODULE_NAME "drivers/disk/virtual/encrypted_disk/device_v1"
#define ENCRYPTED_DISK_CONTROL_MODULE_NAME "drivers/disk/virtual/encrypted_disk/control_v1"

// Magic values and header structure
static const char kEncryptedMagic[] = "HAIKUCR\x01";
static const uint32 kEncryptedVersion = 1;
static const uint32 kHeaderSize = 4096; // 4KiB for better alignment and future expansion

struct encrypted_disk_header {
	char magic[8];
	uint32 version;
	uint32 cipher_type;
	uint32 key_size;
	uint32 block_size;
	uint64 data_size;
	uint8 salt[32];
	uint8 iv[16];
	uint8 validation_hash[32]; // SHA256 hash of encrypted validation string
	uint8 reserved[408];
} _PACKED;

// Compile-time safety check to ensure header fits in reserved space
static_assert(sizeof(encrypted_disk_header) <= kHeaderSize,
	"Header structure is larger than reserved space - increase kHeaderSize");

// Encryption parameters
enum cipher_type { CIPHER_AES256_XTS = 1 };

// Validation string that gets encrypted and stored in the header
static const char kValidationString[] = "HAIKU_ENCRYPTED_DISK_VALID";

static device_manager_info* gDeviceManager;

// Forward declarations
struct encrypted_disk_device;
struct encrypted_partition_info;

// Control device class (following ram_disk pattern)
struct ControlDevice {
	device_node* fNode;

	ControlDevice(device_node* node)
		:
		fNode(node)
	{
	}

	status_t PublishDevice()
	{
		return gDeviceManager->publish_device(fNode, "disk/virtual/encrypted_disk/control",
			ENCRYPTED_DISK_CONTROL_MODULE_NAME);
	}
};

// Global list of encrypted partitions
static DoublyLinkedList<encrypted_partition_info> sEncryptedPartitions;
static mutex sPartitionListLock = MUTEX_INITIALIZER("encrypted partition list");
static int32 sNextDeviceID = 0;

// Control device state
static bool sControlDeviceOpen = false;
static mutex sControlDeviceLock = MUTEX_INITIALIZER("encrypted control device");
static device_node* sControlDeviceNode = NULL;

// Encrypted partition info (tracked by control device)
struct encrypted_partition_info : DoublyLinkedListLinkImpl<encrypted_partition_info> {
	char partition_path[PATH_MAX];
	device_node* virtual_device_node;
	encrypted_disk_device* device;
	int32 device_id;
	bool is_unlocked;
	encrypted_disk_header header;

	// Crypto state (only valid when is_unlocked is true)
	uint8 encryption_key[64]; // 512-bit key for XTS (64 bytes)
	Kernel::Private::crypto::aes_xts_ctx xts_ctx;

	encrypted_partition_info(const char* path)
		:
		virtual_device_node(nullptr),
		device(nullptr),
		device_id(-1),
		is_unlocked(false)
	{
		strlcpy(partition_path, path, sizeof(partition_path));
		memset(&header, 0, sizeof(header));
		memset(encryption_key, 0, sizeof(encryption_key));
	}
};

// Device structure for each virtual encrypted disk instance
struct encrypted_disk_device : DoublyLinkedListLinkImpl<encrypted_disk_device> {
	device_node* node;
	encrypted_partition_info* partition_info;
	int fd; // File descriptor to the actual encrypted partition
	off_t device_size;
	mutex lock;
	uint8* transfer_buffer;

	// Key management
	uint8 encryption_key[64]; // 512-bit key for XTS (64 bytes)
	bool is_unlocked;

	// Crypto context
	Kernel::Private::crypto::aes_xts_ctx xts_ctx;

	encrypted_disk_device(device_node* _node, encrypted_partition_info* _info)
		:
		node(_node),
		partition_info(_info),
		fd(-1),
		device_size(0),
		transfer_buffer(nullptr),
		is_unlocked(false)
	{
		mutex_init(&lock, "encrypted disk device");
		memset(encryption_key, 0, sizeof(encryption_key));
	}

	~encrypted_disk_device()
	{
		if (fd >= 0)
			close(fd);
		memset(encryption_key, 0, sizeof(encryption_key));
		free(transfer_buffer);
		mutex_destroy(&lock);
	}
};

// Find partition info by path
static encrypted_partition_info*
find_partition_info(const char* path)
{
	TRACE("find_partition_info(%s)\n", path);

	MutexLocker locker(sPartitionListLock);

	for (auto* info = sEncryptedPartitions.Head(); info != nullptr;
		info = sEncryptedPartitions.GetNext(info)) {
		if (strcmp(info->partition_path, path) == 0)
			return info;
	}

	return nullptr;
}

// Control device hooks
static status_t
encrypted_disk_control_open(void* initCookie, const char* name, int flags, void** _cookie)
{
	TRACE("control_open: %s\n", name);

	MutexLocker locker(sControlDeviceLock);

	if (sControlDeviceOpen)
		return B_BUSY;

	sControlDeviceOpen = true;
	*_cookie = NULL; // No per-open state needed

	return B_OK;
}


static status_t
encrypted_disk_control_close(void* cookie)
{
	TRACE("control_close\n");

	MutexLocker locker(sControlDeviceLock);
	sControlDeviceOpen = false;

	return B_OK;
}


static status_t
encrypted_disk_control_free(void* cookie)
{
	TRACE("control_free\n");
	return B_OK;
}

// Helper function to generate validation hash
static status_t
generate_validation_hash(uint8* validation_hash, Kernel::Private::crypto::aes_xts_ctx* xts_ctx)
{
	using namespace Kernel::Private::crypto;

	// Prepare validation string buffer
	uint8 validation_buffer[32];
	memset(validation_buffer, 0, 32);
	memcpy(validation_buffer, kValidationString, strlen(kValidationString));

	// Use a fixed IV for validation data encryption. We use a distinct,
	// non-zero IV for the validation encryption to ensure it's different
	// from any actual user data encryption. Since XTS mode uses sector
	// numbers as IVs for regular data, this distinct IV ensures our
	// validation encryption is unique. We never store the encrypted result
	// directly; instead we store only its SHA256 hash to avoid providing
	// any reversible encrypted data to potential attackers.
	uint64_t validation_iv = 0xDEADC0DEC0FFEE00;
	uint8_t iv[8];
	static_assert(sizeof(validation_iv) == sizeof(iv));
	memcpy(iv, &validation_iv, sizeof(validation_iv));
	aes_xts_reinit(xts_ctx, iv);

	// Encrypt the validation data (32 bytes = 2 * 16-byte blocks)
	for (int i = 0; i < 2; i++)
		Kernel::Private::crypto::aes_xts_encrypt(xts_ctx, validation_buffer + (i * 16));

	// Compute SHA256 hash of the encrypted validation data
	sha256_ctx hash_ctx;
	sha256_init(&hash_ctx);
	sha256_update(&hash_ctx, validation_buffer, sizeof(validation_buffer));
	sha256_final(&hash_ctx, validation_hash);

	// Clear the temporary buffer
	memset(validation_buffer, 0, sizeof(validation_buffer));

	return B_OK;
}

// Helper function to validate encryption key using hash comparison
static status_t
validate_encryption_key(const encrypted_disk_header* header,
	Kernel::Private::crypto::aes_xts_ctx* xts_ctx)
{
	// Check if validation hash appears to be uninitialized (all zeros)
	bool has_validation_hash = false;
	for (int i = 0; i < 32; i++) {
		if (header->validation_hash[i] != 0) {
			has_validation_hash = true;
			break;
		}
	}

	if (!has_validation_hash) {
		TRACE_ERROR("Partition has no validation hash - use 'encrypted-disk init' to add "
					"validation\n");
		return B_NOT_INITIALIZED;
	}

	// Generate the validation hash using the current key and compare with stored hash
	uint8 computed_hash[32];
	status_t result = generate_validation_hash(computed_hash, xts_ctx);
	if (result != B_OK) {
		TRACE_ERROR("Failed to compute validation hash\n");
		return result;
	}

	// Compare the computed hash with the stored hash
	if (memcmp(computed_hash, header->validation_hash, 32) == 0) {
		TRACE("Passphrase validation successful\n");
		return B_OK;
	} else {
		TRACE_ERROR("Passphrase validation failed - incorrect passphrase\n");
		return B_PERMISSION_DENIED;
	}
}

// Helper function to read header with backup fallback
static status_t
read_encrypted_header(int fd, off_t partition_size, encrypted_disk_header* header)
{
	// Try reading primary header from first sector
	ssize_t bytesRead = read_pos(fd, 0, header, sizeof(*header));
	if (bytesRead == sizeof(*header)) {
		// Verify magic
		if (memcmp(header->magic, kEncryptedMagic, strlen(kEncryptedMagic)) == 0) {
			TRACE("Successfully read primary header\n");
			return B_OK;
		}
		TRACE("Primary header magic invalid, trying backup\n");
	} else {
		TRACE("Failed to read primary header, trying backup\n");
	}

	// Try reading backup header from multiple possible locations
	// (matches the write logic in encrypted_disk_control.cpp)
	off_t backup_offsets[] = {
		partition_size - kHeaderSize - 512, // One sector before end (avoid GPT backup)
		partition_size - kHeaderSize - 1024, // Two sectors before end
		partition_size - kHeaderSize - 4096, // One page before end
		partition_size - 2 * kHeaderSize, // Two header sizes from end
		partition_size - kHeaderSize // Original location (last attempt)
	};

	for (int i = 0; i < 5; i++) {
		off_t backup_offset = backup_offsets[i];

		// Skip if offset would be in primary header area
		if (backup_offset < kHeaderSize)
			continue;

		bytesRead = read_pos(fd, backup_offset, header, sizeof(*header));
		if (bytesRead == sizeof(*header)) {
			// Verify magic
			if (memcmp(header->magic, kEncryptedMagic, strlen(kEncryptedMagic)) == 0) {
				TRACE_ALWAYS("Successfully read backup header from offset %" B_PRIdOFF
							 " (primary was damaged)\n",
					backup_offset);
				return B_OK;
			}
		}
	}

	TRACE_ERROR("Failed to read valid backup header from any location\n");

	return B_BAD_VALUE;
}


static status_t
handle_partition_init(encrypted_disk_ioctl_partition_control* control)
{
	TRACE("Initializing new encrypted partition: %s\n", control->partition_path);

	// Check if passphrase was provided
	if (control->data.passphrase.passphrase_length == 0) {
		TRACE_ERROR("No passphrase provided for initialization\n");
		return B_BAD_VALUE;
	}

	// Open the partition for reading and writing
	int fd = open(control->partition_path, O_RDWR);
	if (fd < 0) {
		TRACE_ERROR("Cannot open partition %s: %s\n", control->partition_path, strerror(errno));
		return errno;
	}

	// Get partition size
	off_t partition_size = lseek(fd, 0, SEEK_END);
	if (partition_size < 2 * kHeaderSize) {
		TRACE_ERROR("Partition too small for encrypted disk (need at least %d bytes)\n",
			2 * kHeaderSize);
		close(fd);
		return B_BAD_VALUE;
	}

	TRACE("Partition size: %" B_PRIdOFF " bytes\n", partition_size);

	// Create the encrypted disk header
	encrypted_disk_header header;
	memset(&header, 0, sizeof(header));

	memcpy(header.magic, kEncryptedMagic, sizeof(header.magic));
	header.version = kEncryptedVersion;
	header.cipher_type = CIPHER_AES256_XTS;
	header.key_size = 32; // 256-bit key
	header.block_size = 512;
	// Reserve both first and last sectors for metadata
	header.data_size = partition_size - (2 * kHeaderSize);

	// Generate random salt
	TRACE("Generating salt...\n");
	for (int i = 0; i < 32; i++)
		header.salt[i] = (uint8)((system_time() * i) ^ (system_time() >> 8)) & 0xFF;

	// Initialize IV field (not used for verification, just zeroed)
	memset(header.iv, 0, sizeof(header.iv));

	// Derive encryption key using PBKDF2
	uint8 encryption_key[64];
	status_t result = Kernel::Private::crypto::pbkdf2_sha256(control->data.passphrase.passphrase,
		control->data.passphrase.passphrase_length, header.salt, sizeof(header.salt),
		encryption_key, sizeof(encryption_key), PBKDF2_ITERATIONS);

	if (result != B_OK) {
		TRACE_ERROR("Key derivation failed: %s\n", strerror(result));
		close(fd);
		return result;
	}

	// Initialize XTS crypto context
	Kernel::Private::crypto::aes_xts_ctx xts_ctx;
	if (Kernel::Private::crypto::aes_xts_setkey(&xts_ctx, encryption_key, sizeof(encryption_key))
		!= 0) {
		TRACE_ERROR("Crypto initialization failed\n");
		memset(encryption_key, 0, sizeof(encryption_key));
		close(fd);
		return B_ERROR;
	}

	// Generate validation hash
	result = generate_validation_hash(header.validation_hash, &xts_ctx);
	if (result != B_OK) {
		TRACE_ERROR("Failed to generate validation data\n");
		memset(encryption_key, 0, sizeof(encryption_key));
		Kernel::Private::crypto::aes_xts_zerokey(&xts_ctx);
		close(fd);
		return result;
	}

	// Write header to primary location
	ssize_t bytes_written = pwrite(fd, &header, sizeof(header), 0);
	if (bytes_written != sizeof(header)) {
		TRACE_ERROR("Failed to write primary header: %s\n", strerror(errno));
		memset(encryption_key, 0, sizeof(encryption_key));
		Kernel::Private::crypto::aes_xts_zerokey(&xts_ctx);
		close(fd);
		return B_IO_ERROR;
	}

	// Write backup header to last usable location
	off_t backup_offsets[] = {
		partition_size - kHeaderSize - 512, // One sector before end (avoid GPT backup)
		partition_size - kHeaderSize - 1024, // Two sectors before end
		partition_size - kHeaderSize - 4096, // One page before end
		partition_size - 2 * kHeaderSize // Two header sizes from end
	};

	bool backup_written = false;
	for (int i = 0; i < 4; i++) {
		off_t backup_offset = backup_offsets[i];

		// Skip if offset would be in primary header area
		if (backup_offset < kHeaderSize)
			continue;

		bytes_written = pwrite(fd, &header, sizeof(header), backup_offset);
		if (bytes_written == sizeof(header)) {
			TRACE("Successfully wrote backup header to offset %" B_PRIdOFF "\n", backup_offset);
			backup_written = true;
			break;
		}
	}

	if (!backup_written) {
		TRACE("Warning: Failed to write backup header\n");
		// Don't fail the operation, primary header is sufficient
	}

	// Sync to ensure headers are written
	fsync(fd);

	// Clean up sensitive data
	memset(encryption_key, 0, sizeof(encryption_key));
	Kernel::Private::crypto::aes_xts_zerokey(&xts_ctx);
	close(fd);

	TRACE("Successfully initialized partition with validation data\n");
	TRACE("Data size: %" B_PRIdOFF " bytes\n", header.data_size);
	return B_OK;
}


static status_t
handle_partition_unlock(encrypted_disk_ioctl_partition_control* control)
{
	TRACE("Unlocking partition: %s\n", control->partition_path);

	// Check if partition is already tracked
	encrypted_partition_info* info = find_partition_info(control->partition_path);
	if (info == nullptr) {
		// First time seeing this partition - read and verify header
		int fd = open(control->partition_path, O_RDONLY);
		if (fd < 0) {
			TRACE_ERROR("Cannot open partition %s: %s\n", control->partition_path, strerror(errno));
			return errno;
		}

		// Get partition size
		off_t partition_size = lseek(fd, 0, SEEK_END);
		if (partition_size < 2 * kHeaderSize) {
			TRACE_ERROR("Partition too small for encrypted disk (need at least %d bytes)\n",
				2 * kHeaderSize);
			close(fd);
			return B_BAD_VALUE;
		}

		encrypted_disk_header header;
		status_t result = read_encrypted_header(fd, partition_size, &header);
		close(fd);

		if (result != B_OK) {
			TRACE_ERROR("Failed to read valid header from %s\n", control->partition_path);
			return result;
		}

		// Create new partition info
		info = new(std::nothrow) encrypted_partition_info(control->partition_path);
		if (info == nullptr)
			return B_NO_MEMORY;

		info->header = header;
		info->device_id = atomic_add(&sNextDeviceID, 1);

		MutexLocker locker(sPartitionListLock);
		sEncryptedPartitions.Add(info);
	}

	if (info->is_unlocked) {
		TRACE("Partition already unlocked\n");
		return B_OK;
	}

	// Check if passphrase was provided for unlocking
	if (control->data.passphrase.passphrase_length > 0) {
		TRACE("Deriving encryption key from passphrase\n");

		// Derive encryption key using PBKDF2
		status_t result = Kernel::Private::crypto::pbkdf2_sha256(
			control->data.passphrase.passphrase, control->data.passphrase.passphrase_length,
			info->header.salt, sizeof(info->header.salt), info->encryption_key,
			sizeof(info->encryption_key), PBKDF2_ITERATIONS);

		if (result != B_OK) {
			TRACE_ERROR("Key derivation failed: %s\n", strerror(result));
			return result;
		}

		// Initialize XTS crypto context
		if (Kernel::Private::crypto::aes_xts_setkey(&info->xts_ctx, info->encryption_key,
				sizeof(info->encryption_key))
			!= 0) {
			TRACE_ERROR("Crypto initialization failed\n");
			memset(info->encryption_key, 0, sizeof(info->encryption_key));
			return B_ERROR;
		}

		// Validate the passphrase by decrypting validation data
		result = validate_encryption_key(&info->header, &info->xts_ctx);
		if (result != B_OK) {
			TRACE_ERROR("Passphrase validation failed\n");
			memset(info->encryption_key, 0, sizeof(info->encryption_key));
			Kernel::Private::crypto::aes_xts_zerokey(&info->xts_ctx);
			return result;
		}

		TRACE("Encryption key derived, crypto initialized, and passphrase validated\n");
	}

	// Create device attributes for virtual device
	device_attr attrs[] = {{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "Encrypted Disk"}},
		{"encrypted_disk/partition_path", B_STRING_TYPE, {.string = control->partition_path}},
		{"encrypted_disk/device_id", B_UINT32_TYPE, {.ui32 = static_cast<uint32>(info->device_id)}},
		{"encrypted_disk/device_size", B_UINT64_TYPE, {.ui64 = info->header.data_size}}, {NULL}};

	// Register virtual device as child of control device's parent
	device_node* parent = NULL;
	if (sControlDeviceNode != NULL)
		parent = gDeviceManager->get_parent_node(sControlDeviceNode);

	status_t status = gDeviceManager->register_node(parent, ENCRYPTED_DISK_DRIVER_MODULE_NAME,
		attrs, NULL, &info->virtual_device_node);

	TRACE_ALWAYS("encrypted_disk: registered virtual device with parent %p: %s\n", parent,
		strerror(status));

	if (status != B_OK) {
		TRACE_ERROR("Failed to register virtual device: %s\n", strerror(status));
		// Clean up on failure
		MutexLocker locker(sPartitionListLock);
		sEncryptedPartitions.Remove(info);
		delete info;
		return status;
	}

	// Immediately publish the device like ram_disk does
	if (info->virtual_device_node != nullptr) {
		char path[64];
		snprintf(path, sizeof(path), "disk/virtual/encrypted_disk/%d/raw", info->device_id);

		TRACE_ALWAYS("Publishing virtual device at %s\n", path);
		status = gDeviceManager->publish_device(info->virtual_device_node, path,
			ENCRYPTED_DISK_DEVICE_MODULE_NAME);

		if (status != B_OK) {
			TRACE_ERROR("Failed to publish virtual device: %s\n", strerror(status));
			// Clean up on failure
			gDeviceManager->unregister_node(info->virtual_device_node);
			MutexLocker locker(sPartitionListLock);
			sEncryptedPartitions.Remove(info);
			delete info;
			return status;
		}
	}

	info->is_unlocked = true;
	TRACE("Successfully created virtual device for %s (id=%d)\n", control->partition_path,
		info->device_id);

	return B_OK;
}


static status_t
handle_partition_lock(const char* partition_path)
{
	TRACE("Locking partition: %s\n", partition_path);

	encrypted_partition_info* info = find_partition_info(partition_path);
	if (info == nullptr) {
		TRACE_ERROR("Partition not found: %s\n", partition_path);
		return B_ENTRY_NOT_FOUND;
	}

	if (!info->is_unlocked) {
		TRACE("Partition already locked\n");
		return B_OK;
	}

	// Unpublish and unregister virtual device
	if (info->virtual_device_node != nullptr) {
		// First unpublish the device from devfs
		char path[64];
		snprintf(path, sizeof(path), "disk/virtual/encrypted_disk/%d/raw", info->device_id);
		TRACE("Unpublishing device at %s\n", path);
		gDeviceManager->unpublish_device(info->virtual_device_node, path);

		// Then unregister the node
		gDeviceManager->unregister_node(info->virtual_device_node);
		info->virtual_device_node = nullptr;
	}

	// Clear encryption keys and crypto context for security
	memset(info->encryption_key, 0, sizeof(info->encryption_key));
	Kernel::Private::crypto::aes_xts_zerokey(&info->xts_ctx);

	info->is_unlocked = false;

	// Don't delete the partition info here - keep it in the list
	// so subsequent unlock operations can reuse it. The info will
	// be cleaned up when the driver unloads or the system shuts down.

	TRACE("Partition locked successfully\n");
	return B_OK;
}


static status_t
handle_partition_status(const char* partition_path, encrypted_disk_ioctl_status* status)
{
	TRACE("Getting status for partition: %s\n", partition_path);

	// Try to read header directly from partition
	int fd = open(partition_path, O_RDONLY);
	if (fd < 0) {
		TRACE_ERROR("Cannot open partition %s: %s\n", partition_path, strerror(errno));
		return errno;
	}

	// Get partition size for backup header location
	off_t partition_size = lseek(fd, 0, SEEK_END);

	encrypted_disk_header header;
	status_t result = read_encrypted_header(fd, partition_size, &header);
	close(fd);

	if (result != B_OK) {
		TRACE_ERROR("Failed to read valid header from %s\n", partition_path);
		return result;
	}

	// Successfully read header - it's an encrypted partition

	// Check if partition is unlocked
	encrypted_partition_info* info = find_partition_info(partition_path);

	status->status = (info != nullptr && info->is_unlocked) ? ENCRYPTED_DISK_STATUS_UNLOCKED
															: ENCRYPTED_DISK_STATUS_LOCKED;
	status->device_size = header.data_size;
	status->cipher_type = header.cipher_type;

	return B_OK;
}


static status_t
encrypted_disk_control_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	TRACE("control_ioctl: op=0x%x\n", op);

	switch (op) {
		case ENCRYPTED_DISK_IOCTL_UNLOCK_PARTITION:
		{
			if (length != sizeof(encrypted_disk_ioctl_partition_control))
				return B_BAD_VALUE;

			encrypted_disk_ioctl_partition_control control;
			if (IS_USER_ADDRESS(buffer)) {
				if (user_memcpy(&control, buffer, sizeof(control)) != B_OK)
					return B_BAD_ADDRESS;
			} else {
				memcpy(&control, buffer, sizeof(control));
			}

			return handle_partition_unlock(&control);
		}

		case ENCRYPTED_DISK_IOCTL_LOCK_PARTITION:
		{
			if (length != sizeof(encrypted_disk_ioctl_partition_control))
				return B_BAD_VALUE;

			encrypted_disk_ioctl_partition_control control;
			if (IS_USER_ADDRESS(buffer)) {
				if (user_memcpy(&control, buffer, sizeof(control)) != B_OK)
					return B_BAD_ADDRESS;
			} else {
				memcpy(&control, buffer, sizeof(control));
			}

			return handle_partition_lock(control.partition_path);
		}

		case ENCRYPTED_DISK_IOCTL_STATUS_PARTITION:
		{
			if (length != sizeof(encrypted_disk_ioctl_partition_control))
				return B_BAD_VALUE;

			encrypted_disk_ioctl_partition_control control;
			if (IS_USER_ADDRESS(buffer)) {
				if (user_memcpy(&control, buffer, sizeof(control)) != B_OK)
					return B_BAD_ADDRESS;
			} else {
				memcpy(&control, buffer, sizeof(control));
			}

			status_t result = handle_partition_status(control.partition_path, &control.data.status);

			if (result == B_OK) {
				if (IS_USER_ADDRESS(buffer))
					return user_memcpy(buffer, &control, sizeof(control));
				else
					memcpy(buffer, &control, sizeof(control));
			}

			return result;
		}

		case ENCRYPTED_DISK_IOCTL_INIT_PARTITION:
		{
			if (length != sizeof(encrypted_disk_ioctl_partition_control))
				return B_BAD_VALUE;

			encrypted_disk_ioctl_partition_control control;
			if (IS_USER_ADDRESS(buffer)) {
				if (user_memcpy(&control, buffer, sizeof(control)) != B_OK)
					return B_BAD_ADDRESS;
			} else {
				memcpy(&control, buffer, sizeof(control));
			}

			return handle_partition_init(&control);
		}

		default:
			return B_DEV_INVALID_IOCTL;
	}
}

// Virtual device hooks
static status_t
encrypted_disk_init_device(void* driverCookie, void** _cookie)
{
	TRACE("init_device\n");

	// Driver cookie contains the device_id
	int32 device_id = static_cast<int32>(reinterpret_cast<addr_t>(driverCookie));

	// Find the partition info
	encrypted_partition_info* info = nullptr;
	{
		MutexLocker locker(sPartitionListLock);
		for (auto* p = sEncryptedPartitions.Head(); p != nullptr;
			p = sEncryptedPartitions.GetNext(p)) {
			if (p->device_id == device_id) {
				info = p;
				break;
			}
		}
	}

	if (info == nullptr) {
		TRACE_ERROR("Device ID %d not found\n", device_id);
		return B_ENTRY_NOT_FOUND;
	}

	// Open the actual encrypted partition
	int fd = open(info->partition_path, O_RDWR);
	if (fd < 0) {
		TRACE_ERROR("Cannot open partition %s: %s\n", info->partition_path, strerror(errno));
		return errno;
	}

	// Create device structure
	encrypted_disk_device* device = new(std::nothrow) encrypted_disk_device(nullptr, info);
	if (device == nullptr) {
		close(fd);
		return B_NO_MEMORY;
	}

	device->fd = fd;
	info->device = device;

	// Use the data_size from the header which was calculated during partition initialization
	device->device_size = info->header.data_size;

	TRACE_ALWAYS("Virtual device size: %ld bytes (from header data_size)\n", device->device_size);

	// Synchronize device state with partition state
	if (info->is_unlocked) {
		TRACE_ALWAYS("Synchronizing device with already-unlocked partition (device_id=%d)\n",
			device_id);
		device->is_unlocked = true;

		// Copy the encryption key and crypto context
		memcpy(device->encryption_key, info->encryption_key, sizeof(device->encryption_key));
		memcpy(&device->xts_ctx, &info->xts_ctx, sizeof(device->xts_ctx));
	} else {
		TRACE_ALWAYS("Device initialized for locked partition (device_id=%d)\n", device_id);
	}

	*_cookie = device;
	return B_OK;
}


static void
encrypted_disk_uninit_device(void* _cookie)
{
	TRACE("uninit_device\n");

	encrypted_disk_device* device = static_cast<encrypted_disk_device*>(_cookie);

	// Clean up resources to prevent leaks and unmount hangs
	if (device->fd >= 0) {
		TRACE("Closing file descriptor %d\n", device->fd);
		close(device->fd);
		device->fd = -1;
	}

	if (device->transfer_buffer != nullptr) {
		TRACE("Freeing transfer buffer\n");
		free(device->transfer_buffer);
		device->transfer_buffer = nullptr;
	}

	// Clear encryption keys from memory for security
	memset(device->encryption_key, 0, sizeof(device->encryption_key));

	if (device->partition_info)
		device->partition_info->device = nullptr;

	TRACE("Device cleanup complete\n");
	delete device;
}


static status_t
encrypted_disk_open(void* _cookie, const char* path, int openMode, void** _newCookie)
{
	TRACE("open: %s\n", path);

	encrypted_disk_device* device = static_cast<encrypted_disk_device*>(_cookie);
	*_newCookie = device;

	return B_OK;
}


static status_t
encrypted_disk_close(void* cookie)
{
	TRACE("close\n");
	return B_OK;
}


static status_t
encrypted_disk_free(void* cookie)
{
	TRACE("free\n");
	return B_OK;
}


static status_t
encrypted_disk_read(void* cookie, off_t pos, void* buffer, size_t* length)
{
	encrypted_disk_device* device = static_cast<encrypted_disk_device*>(cookie);

	TRACE("read: pos=%ld, length=%zu, is_unlocked=%s, device_size=%ld\n", pos, *length,
		device->is_unlocked ? "YES" : "NO", device->device_size);

	if (!device->is_unlocked) {
		TRACE_ERROR("Attempt to read from locked device\n");
		return B_PERMISSION_DENIED;
	}

	MutexLocker locker(device->lock);

	// Ensure we don't read past the end of the device
	if (pos >= device->device_size) {
		*length = 0;
		return B_OK;
	}

	if (pos + static_cast<off_t>(*length) > device->device_size)
		*length = device->device_size - pos;

	// Calculate sector-aligned boundaries
	off_t sector_size = 512;
	off_t start_sector = pos / sector_size;
	off_t end_sector = (pos + *length + sector_size - 1) / sector_size;
	off_t sectors_to_read = end_sector - start_sector;
	off_t bytes_to_read = sectors_to_read * sector_size;

	// Ensure we don't read beyond the device boundary when accounting for header offset
	off_t max_read_end = device->device_size;
	if (start_sector * sector_size + bytes_to_read > max_read_end) {
		bytes_to_read = max_read_end - (start_sector * sector_size);
		// Round down to sector boundary
		bytes_to_read = (bytes_to_read / sector_size) * sector_size;
		TRACE("Adjusted read size to prevent boundary overflow: bytes_to_read=%ld\n",
			bytes_to_read);
	}

	// Allocate temporary buffer for sector-aligned read (16-byte aligned for AES-NI)
	if (device->transfer_buffer == nullptr) {
		device->transfer_buffer
			= static_cast<uint8*>(memalign(16, 64 * 1024)); // 64KB buffer, 16-byte aligned
		if (device->transfer_buffer == nullptr)
			return B_NO_MEMORY;
	}

	size_t total_read = 0;
	off_t current_pos = start_sector * sector_size;

	while (total_read < *length) {
		// Read up to buffer size at a time
		size_t chunk_size = std::min(static_cast<size_t>(bytes_to_read - total_read),
			static_cast<size_t>(64 * 1024));
		size_t aligned_chunk = (chunk_size / sector_size) * sector_size;

		// Read encrypted data from underlying partition
		// Add header offset to position
		off_t read_pos_actual = kHeaderSize + current_pos;
		TRACE("Reading from underlying partition: pos=%ld, size=%zu (virtual_pos=%ld)\n",
			read_pos_actual, aligned_chunk, current_pos);

		ssize_t bytes_read
			= read_pos(device->fd, read_pos_actual, device->transfer_buffer, aligned_chunk);

		if (bytes_read < 0) {
			TRACE_ERROR("Read error at pos=%ld: %s\n", read_pos_actual, strerror(errno));
			return errno;
		}

		if (bytes_read == 0)
			break;

		// Decrypt sectors
		for (off_t i = 0; i < bytes_read; i += sector_size) {
			uint64_t sector_num = (current_pos + i) / sector_size;

			// Reinit tweak for this sector
			uint8_t iv[8];
			memcpy(iv, &sector_num, sizeof(sector_num));
			Kernel::Private::crypto::aes_xts_reinit(&device->xts_ctx, iv);

			// Decrypt sector (512 bytes = 32 * 16-byte blocks)
			for (int j = 0; j < 32; j++) {
				Kernel::Private::crypto::aes_xts_decrypt(&device->xts_ctx,
					device->transfer_buffer + i + (j * 16));
			}
		}

		// Copy the requested portion to the output buffer
		off_t offset_in_first_sector = (total_read == 0) ? (pos % sector_size) : 0;
		size_t copy_size = std::min(static_cast<size_t>(bytes_read - offset_in_first_sector),
			*length - total_read);

		status_t copy_result = user_memcpy(static_cast<uint8*>(buffer) + total_read,
			device->transfer_buffer + offset_in_first_sector, copy_size);
		if (copy_result != B_OK) {
			TRACE_ERROR("Failed to copy data to userspace: %s\n", strerror(copy_result));
			return copy_result;
		}

		total_read += copy_size;
		current_pos += bytes_read;
	}

	*length = total_read;
	return B_OK;
}


static status_t
encrypted_disk_write(void* cookie, off_t pos, const void* buffer, size_t* length)
{
	encrypted_disk_device* device = static_cast<encrypted_disk_device*>(cookie);

	if (!device->is_unlocked) {
		TRACE_ERROR("Attempt to write to locked device\n");
		return B_PERMISSION_DENIED;
	}

	MutexLocker locker(device->lock);

	// Ensure we don't write past the end of the device
	if (pos >= device->device_size) {
		*length = 0;
		return B_OK;
	}

	if (pos + static_cast<off_t>(*length) > device->device_size)
		*length = device->device_size - pos;

	// Calculate sector-aligned boundaries
	off_t sector_size = 512;
	off_t start_sector = pos / sector_size;
	off_t end_sector = (pos + *length + sector_size - 1) / sector_size;
	off_t sectors_to_write = end_sector - start_sector;

	// Allocate temporary buffer for sector-aligned operations (16-byte aligned for AES-NI)
	if (device->transfer_buffer == nullptr) {
		device->transfer_buffer
			= static_cast<uint8*>(memalign(16, 64 * 1024)); // 64KB buffer, 16-byte aligned
		if (device->transfer_buffer == nullptr)
			return B_NO_MEMORY;
	}

	size_t total_written = 0;
	off_t current_pos = start_sector * sector_size;

	while (total_written < *length) {
		// Process up to buffer size at a time
		size_t chunk_sectors = std::min(
			static_cast<size_t>(sectors_to_write - (current_pos / sector_size - start_sector)),
			static_cast<size_t>(64 * 1024 / sector_size));
		size_t chunk_size = chunk_sectors * sector_size;

		// Handle partial first/last sectors
		bool need_read = false;
		off_t offset_in_first_sector = (total_written == 0) ? (pos % sector_size) : 0;
		size_t bytes_in_chunk = std::min(static_cast<size_t>(chunk_size - offset_in_first_sector),
			*length - total_written);

		// Check if we need to read-modify-write
		if (offset_in_first_sector != 0
			|| (bytes_in_chunk < chunk_size
				&& current_pos + static_cast<off_t>(chunk_size) <= device->device_size)) {
			need_read = true;
		}

		if (need_read) {
			// Read existing encrypted data
			ssize_t bytes_read = read_pos(device->fd, kHeaderSize + current_pos,
				device->transfer_buffer, chunk_size);

			if (bytes_read < 0) {
				TRACE_ERROR("Read error during RMW: %s\n", strerror(errno));
				return errno;
			}

			// Decrypt existing data
			for (off_t i = 0; i < bytes_read; i += sector_size) {
				uint64_t sector_num = (current_pos + i) / sector_size;

				// Reinit tweak for this sector
				uint8_t iv[8];
				memcpy(iv, &sector_num, sizeof(sector_num));
				Kernel::Private::crypto::aes_xts_reinit(&device->xts_ctx, iv);

				// Decrypt sector (512 bytes = 32 * 16-byte blocks)
				for (int j = 0; j < 32; j++) {
					Kernel::Private::crypto::aes_xts_decrypt(&device->xts_ctx,
						device->transfer_buffer + i + (j * 16));
				}
			}
		}

		// Copy new data from userspace into kernel buffer
		status_t copy_result = user_memcpy(device->transfer_buffer + offset_in_first_sector,
			static_cast<const uint8*>(buffer) + total_written, bytes_in_chunk);
		if (copy_result != B_OK) {
			TRACE_ERROR("Failed to copy data from userspace: %s\n", strerror(copy_result));
			return copy_result;
		}

		// Encrypt sectors
		for (off_t i = 0; i < static_cast<off_t>(chunk_size); i += sector_size) {
			uint64_t sector_num = (current_pos + i) / sector_size;

			// Reinit tweak for this sector
			uint8_t iv[8];
			memcpy(iv, &sector_num, sizeof(sector_num));
			Kernel::Private::crypto::aes_xts_reinit(&device->xts_ctx, iv);

			// Encrypt sector (512 bytes = 32 * 16-byte blocks)
			for (int j = 0; j < 32; j++) {
				Kernel::Private::crypto::aes_xts_encrypt(&device->xts_ctx,
					device->transfer_buffer + i + (j * 16));
			}
		}

		// Write encrypted data
		ssize_t bytes_written
			= write_pos(device->fd, kHeaderSize + current_pos, device->transfer_buffer, chunk_size);

		if (bytes_written < 0) {
			TRACE_ERROR("Write error: %s\n", strerror(errno));
			return errno;
		}

		if (bytes_written != static_cast<ssize_t>(chunk_size)) {
			TRACE_ERROR("Short write: %ld vs %ld\n", bytes_written, chunk_size);
			return B_IO_ERROR;
		}

		total_written += bytes_in_chunk;
		current_pos += chunk_size;
	}

	*length = total_written;
	return B_OK;
}


static status_t
encrypted_disk_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	encrypted_disk_device* device = static_cast<encrypted_disk_device*>(cookie);

	TRACE("ioctl: op=0x%x, length=%zu, is_unlocked=%s\n", op, length,
		device->is_unlocked ? "YES" : "NO");

	switch (op) {
		case B_GET_DEVICE_SIZE:
		{
			if (length != sizeof(size_t))
				return B_BAD_VALUE;
			size_t size = static_cast<size_t>(device->device_size);
			return user_memcpy(buffer, &size, sizeof(size_t));
		}

		case B_SET_NONBLOCKING_IO:
		case B_SET_BLOCKING_IO:
			return B_OK;

		case B_GET_READ_STATUS:
		case B_GET_WRITE_STATUS:
		{
			// Always return true for device readiness so DriveSetup can see the device
			// Access control is handled at the read/write operation level
			bool value = true;
			return user_memcpy(buffer, &value, sizeof(bool));
		}

		case B_GET_GEOMETRY:
		case B_GET_BIOS_GEOMETRY:
		{
			if (buffer == NULL || length > sizeof(device_geometry))
				return B_BAD_VALUE;

			device_geometry geometry;
			memset(&geometry, 0, sizeof(geometry));

			// Use proper devfs geometry calculation like nvme_disk
			uint64_t capacity = device->device_size / 512; // Convert to sectors
			devfs_compute_geometry_size(&geometry, capacity, 512);

			// Set device-specific properties
			geometry.device_type = B_DISK;
			geometry.removable = false; // encrypted disks are not removable
			geometry.read_only = false;
			geometry.write_once = false;
			geometry.bytes_per_physical_sector = 512;

			return user_memcpy(buffer, &geometry, length);
		}

		case B_GET_MEDIA_STATUS:
		{
			// Always return B_OK for media status so the device appears in DriveSetup
			// The device exists whether locked or unlocked - access control is handled
			// at the read/write level
			status_t media_status = B_OK;
			return user_memcpy(buffer, &media_status, sizeof(status_t));
		}

		case B_GET_ICON_NAME:
			return user_strlcpy((char*)buffer, "devices/drive-encrypted", B_FILE_NAME_LENGTH);

		case B_GET_VECTOR_ICON:
		{
			// For now, return an error - we could add a proper encrypted disk icon later
			return B_NOT_SUPPORTED;
		}

		case B_SET_UNINTERRUPTABLE_IO:
		case B_SET_INTERRUPTABLE_IO:
		case B_FLUSH_DRIVE_CACHE:
			return B_OK;

		case B_TRIM_DEVICE:
		{
			// We don't support TRIM for encrypted disks - data would be lost
			return B_NOT_SUPPORTED;
		}

		case B_EJECT_DEVICE:
		case B_LOAD_MEDIA:
		{
			// Encrypted disks cannot be ejected or loaded like removable media
			return B_NOT_SUPPORTED;
		}

		case ENCRYPTED_DISK_IOCTL_SET_PASSPHRASE:
		{
			if (length != sizeof(encrypted_disk_ioctl_set_passphrase))
				return B_BAD_VALUE;

			encrypted_disk_ioctl_set_passphrase passphrase_data;
			if (IS_USER_ADDRESS(buffer)) {
				if (user_memcpy(&passphrase_data, buffer, sizeof(passphrase_data)) != B_OK)
					return B_BAD_ADDRESS;
			} else {
				memcpy(&passphrase_data, buffer, sizeof(passphrase_data));
			}

			// Derive encryption key using PBKDF2
			status_t result = Kernel::Private::crypto::pbkdf2_sha256(passphrase_data.passphrase,
				passphrase_data.passphrase_length, device->partition_info->header.salt,
				sizeof(device->partition_info->header.salt), device->encryption_key,
				sizeof(device->encryption_key), PBKDF2_ITERATIONS);

			if (result != B_OK) {
				TRACE_ERROR("Key derivation failed: %s\n", strerror(result));
				return result;
			}

			// Initialize XTS crypto context
			if (Kernel::Private::crypto::aes_xts_setkey(&device->xts_ctx, device->encryption_key,
					sizeof(device->encryption_key))
				!= 0) {
				TRACE_ERROR("Crypto initialization failed\n");
				memset(device->encryption_key, 0, sizeof(device->encryption_key));
				return B_ERROR;
			}

			// Validate the passphrase by decrypting validation data
			result = validate_encryption_key(&device->partition_info->header, &device->xts_ctx);
			if (result != B_OK) {
				TRACE_ERROR("Passphrase validation failed\n");
				memset(device->encryption_key, 0, sizeof(device->encryption_key));
				Kernel::Private::crypto::aes_xts_zerokey(&device->xts_ctx);
				return result;
			}

			device->is_unlocked = true;

			// Clear passphrase from local memory
			memset(&passphrase_data, 0, sizeof(passphrase_data));

			TRACE("Device unlocked successfully\n");
			return B_OK;
		}

		case ENCRYPTED_DISK_IOCTL_GET_STATUS:
		{
			if (length != sizeof(encrypted_disk_ioctl_status))
				return B_BAD_VALUE;

			encrypted_disk_ioctl_status status;
			status.status = device->is_unlocked ? ENCRYPTED_DISK_STATUS_UNLOCKED
												: ENCRYPTED_DISK_STATUS_LOCKED;
			status.device_size = device->device_size;
			status.cipher_type = device->partition_info->header.cipher_type;

			return user_memcpy(buffer, &status, sizeof(status));
		}

		default:
			TRACE_ALWAYS("Unknown ioctl opcode 0x%x (%u) called on encrypted disk device\n", op,
				op);
			if (op == 0x8000) {
				TRACE_ALWAYS("This is B_AUDIO_DRIVER_BASE - DriveSetup is calling audio driver "
							 "ioctl on disk device!\n");
			}
			return B_DEV_INVALID_IOCTL;
	}
}

// Driver module hooks
static float
encrypted_disk_supports_device(device_node* parent)
{
	TRACE_ALWAYS("encrypted_disk: supports_device called (parent=%p)\n", parent);

	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK) {
		TRACE_ALWAYS("encrypted_disk: supports_device: no bus attribute\n");
		return -1;
	}

	TRACE_ALWAYS("encrypted_disk: supports_device: bus = %s\n", bus);

	// Support the generic bus for control device (like ram_disk)
	if (strcmp(bus, "generic") == 0) {
		TRACE_ALWAYS("encrypted_disk: supports_device: supporting generic bus with priority 0.8\n");
		return 0.8; // Same priority as ram_disk
	}

	// Also support our virtual encrypted devices
	if (strcmp(bus, "virtual") == 0) {
		uint32 device_id;
		if (gDeviceManager->get_attr_uint32(parent, "encrypted_disk/device_id", &device_id, false)
			== B_OK) {
			TRACE_ALWAYS("encrypted_disk: supports_device: supporting virtual device %d\n",
				device_id);
			return 1.0;
		}
	}

	TRACE_ALWAYS("encrypted_disk: supports_device: not supporting bus %s\n", bus);
	return -1;
}


static status_t
encrypted_disk_register_device(device_node* parent)
{
	TRACE_ALWAYS("encrypted_disk: register_device called (parent=%p)\n", parent);

	const char* bus;
	status_t busResult = gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false);
	if (busResult == B_OK) {
		TRACE_ALWAYS("encrypted_disk: register_device: bus = %s\n", bus);
		// Handle generic bus - create control device (like ram_disk)
		if (strcmp(bus, "generic") == 0) {
			TRACE_ALWAYS(
				"encrypted_disk: register_device: registering control device on generic bus\n");
			device_attr attrs[]
				= {{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "Encrypted Disk Control"}},
					{NULL}};

			// This creates a permanent device node that will be published
			// and keeps the driver loaded, just like ram_disk does
			status_t result = gDeviceManager->register_node(parent,
				ENCRYPTED_DISK_DRIVER_MODULE_NAME, attrs, NULL, NULL);
			TRACE_ALWAYS("encrypted_disk: register_device: register_node result = %s\n",
				strerror(result));
			return result;
		}
	} else {
		TRACE_ALWAYS("encrypted_disk: register_device: get_attr_string(B_DEVICE_BUS) failed: %s\n",
			strerror(busResult));
	}

	// Handle virtual encrypted devices - just register the node, don't publish yet
	uint32 device_id;
	if (gDeviceManager->get_attr_uint32(parent, "encrypted_disk/device_id", &device_id, false)
		!= B_OK) {
		TRACE_ALWAYS("encrypted_disk: register_device: no device_id found\n");
		return B_ERROR;
	}

	TRACE_ALWAYS("encrypted_disk: register_device: registering virtual device %d\n", device_id);
	// For virtual devices, just register them - publication happens in register_child_devices
	return B_OK;
}


static status_t
encrypted_disk_init_driver(device_node* node, void** _driverCookie)
{
	TRACE("init_driver\n");

	// Check if we have a device_id attribute to distinguish between
	// control device and raw devices (following ram_disk pattern)
	uint32 device_id;
	if (gDeviceManager->get_attr_uint32(node, "encrypted_disk/device_id", &device_id, false)
		== B_OK) {
		// This is a raw encrypted device
		*_driverCookie = reinterpret_cast<void*>(static_cast<addr_t>(device_id));
	} else {
		// This is the control device (no device_id attribute)
		// Store the control device node globally for ioctl access
		sControlDeviceNode = node;

		// Create a ControlDevice object to manage it
		ControlDevice* device = new(std::nothrow) ControlDevice(node);
		if (device == NULL)
			return B_NO_MEMORY;

		*_driverCookie = device;
	}

	return B_OK;
}


static void
encrypted_disk_uninit_driver(void* driverCookie)
{
	TRACE("uninit_driver\n");
	// If this is a control device, delete it
	ControlDevice* device = dynamic_cast<ControlDevice*>((ControlDevice*)driverCookie);
	if (device != NULL)
		delete device;
}


static status_t
encrypted_disk_register_child_devices(void* _driverCookie)
{
	TRACE("register_child_devices\n");

	// Check if this is a control device by trying to see if the pointer
	// looks like a valid ControlDevice object. For virtual devices,
	// the cookie is just an integer cast to a pointer.

	// If the value is small (< 1000), it's likely a device_id, not a pointer
	addr_t cookieValue = reinterpret_cast<addr_t>(_driverCookie);
	if (cookieValue < 1000) {
		// This is a virtual device - but devices are now published during unlock
		// so there's nothing to do here
		TRACE("register_child_devices for virtual device %d - already published\n",
			static_cast<int32>(cookieValue));
		return B_OK;
	} else {
		// This should be a control device
		ControlDevice* device = static_cast<ControlDevice*>(_driverCookie);
		return device->PublishDevice();
	}
}


// Module initialization
static status_t
encrypted_disk_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			TRACE_ALWAYS("encrypted_disk: module init (B_MODULE_INIT)\n");
			// The driver should stay loaded after init
			TRACE_ALWAYS("encrypted_disk: module init complete, driver loaded\n");
			return B_OK;

		case B_MODULE_UNINIT:
			TRACE_ALWAYS("encrypted_disk: module uninit (B_MODULE_UNINIT)\n");
			// TODO: Clean up control device and any active devices
			return B_OK;

		default:
			TRACE_ALWAYS("encrypted_disk: unknown module op %d\n", (int)op);
			return B_ERROR;
	}
}

// Device module info structures
static device_module_info sEncryptedDiskDeviceModule = {
	{ENCRYPTED_DISK_DEVICE_MODULE_NAME, 0, encrypted_disk_std_ops},

	encrypted_disk_init_device, encrypted_disk_uninit_device,
	NULL, // remove

	encrypted_disk_open, encrypted_disk_close, encrypted_disk_free, encrypted_disk_read,
	encrypted_disk_write,
	NULL, // io
	encrypted_disk_ioctl,

	NULL, // select
	NULL, // deselect
};

static device_module_info sEncryptedDiskControlModule = {
	{ENCRYPTED_DISK_CONTROL_MODULE_NAME, 0, encrypted_disk_std_ops},

	NULL, // init_device
	NULL, // uninit_device
	NULL, // remove

	encrypted_disk_control_open, encrypted_disk_control_close, encrypted_disk_control_free,
	NULL, // read
	NULL, // write
	NULL, // io
	encrypted_disk_control_ioctl,

	NULL, // select
	NULL, // deselect
};

static driver_module_info sEncryptedDiskDriverModule = {
	{ENCRYPTED_DISK_DRIVER_MODULE_NAME, 0, encrypted_disk_std_ops},

	encrypted_disk_supports_device, encrypted_disk_register_device, encrypted_disk_init_driver,
	encrypted_disk_uninit_driver, encrypted_disk_register_child_devices,
	NULL, // rescan
	NULL, // removed
};


module_dependency module_dependencies[]
	= {{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager}, {}};

module_info* modules[] = {(module_info*)&sEncryptedDiskDriverModule,
	(module_info*)&sEncryptedDiskDeviceModule, (module_info*)&sEncryptedDiskControlModule, NULL};
