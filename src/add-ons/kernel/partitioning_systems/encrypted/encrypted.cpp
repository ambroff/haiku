#include <KernelExport.h>
#include <disk_device_manager/ddm_modules.h>
#include <module.h>
#include <util/kernel_cpp.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


// #define TRACE_ENCRYPTED
#ifdef TRACE_ENCRYPTED
#define TRACE(x...) dprintf("encrypted: " x)
#else
#define TRACE(x...) ;
#endif
#define TRACE_ERROR(x...) dprintf("encrypted: " x)


// Magic values and header structure
static const char kEncryptedMagic[] = "HAIKUCR\x01";
static const uint32 kEncryptedVersion = 1;
static const uint32 kHeaderSize = 512;

struct encrypted_disk_header {
	char magic[8];
	uint32 version;
	uint32 cipher_type;
	uint32 key_size;
	uint32 block_size;
	uint64 data_size;
	uint8 salt[32];
	uint8 iv[16];
	uint8 reserved[440];
} _PACKED;


static float
encrypted_identify_partition(int fd, partition_data* partition, void** _cookie)
{
	TRACE("*** IDENTIFY_PARTITION CALLED *** fd=%d, offset=%" B_PRIdOFF ", size=%" B_PRIdOFF "\n",
		fd, partition->offset, partition->size);

	// Also check if partition has type information
#ifdef TRACE_ENCRYPTED
	if (partition->type)
		TRACE("identify_partition: partition type = '%s'\n", partition->type);
	else
		TRACE("identify_partition: no partition type set\n");
#endif

	// Read the encrypted disk header from the beginning of the partition
	// Note: fd should be opened to this specific partition, so we read from offset 0
	encrypted_disk_header header;
	ssize_t bytesRead = read_pos(fd, 0, &header, sizeof(header));

	if (bytesRead != sizeof(header)) {
		TRACE("identify_partition: failed to read header (got %zd bytes, expected %zu)\n",
			bytesRead, sizeof(header));
		return -1;
	}

	TRACE("identify_partition: successfully read %zd bytes from partition start (offset 0)\n",
		bytesRead);

	// Debug: show what we actually read
	TRACE("identify_partition: first 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
		  "%02x %02x %02x %02x %02x %02x\n",
		header.magic[0], header.magic[1], header.magic[2], header.magic[3], header.magic[4],
		header.magic[5], header.magic[6], header.magic[7],
		reinterpret_cast<uint8*>(&header.version)[0], reinterpret_cast<uint8*>(&header.version)[1],
		reinterpret_cast<uint8*>(&header.version)[2], reinterpret_cast<uint8*>(&header.version)[3],
		reinterptet_cast<uint8*>(&header.cipher_type)[0],
		reinterpret_cast<uint8*>(&header.cipher_type)[1],
		reinterpret_cast<uint8*>(&header.cipher_type)[2],
		reinterpret_cast<uint8*>(&header.cipher_type)[3]);

	// Debug: show expected magic
	TRACE("identify_partition: expected magic: %02x %02x %02x %02x %02x %02x %02x %02x ('%s')\n",
		reinterpret_cast<uint8>(kEncryptedMagic[0]), reinterpret_cast<uint8>(kEncryptedMagic[1]),
		reinterpret_cast<uint8>(kEncryptedMagic[2]), reinterpret_cast<uint8>(kEncryptedMagic[3]),
		reinterpret_cast<uint8>(kEncryptedMagic[4]), reinterpret_cast<uint8>(kEncryptedMagic[5]),
		reinterpret_cast<uint8>(kEncryptedMagic[6]), reinterpret_cast<uint8>(kEncryptedMagic[7]),
		kEncryptedMagic);

	// Check for our magic signature
	if (memcmp(header.magic, kEncryptedMagic, strlen(kEncryptedMagic)) != 0) {
		TRACE("identify_partition: no encrypted magic found - magic mismatch\n");
		return -1;
	}

	// Check version
	if (header.version != kEncryptedVersion) {
		TRACE_ERROR("identify_partition: unsupported version %u\n", header.version);
		return -1;
	}

	TRACE(
		"identify_partition: Found encrypted partition! version=%u, cipher=%u, data_size=%" B_PRIu64
		"\n",
		header.version, header.cipher_type, header.data_size);

	// Create a cookie to pass to scan_partition
	encrypted_disk_header* cookie = (encrypted_disk_header*)malloc(sizeof(encrypted_disk_header));
	if (cookie == NULL)
		return -1;

	memcpy(cookie, &header, sizeof(header));
	*_cookie = cookie;

	// Return high priority to handle this partition
	return 0.9f;
}


static status_t
encrypted_scan_partition(int fd, partition_data* partition, void* _cookie)
{
	TRACE("scan_partition: setting up encrypted partition\n");

	encrypted_disk_header* header = (encrypted_disk_header*)_cookie;
	if (header == NULL)
		return B_BAD_VALUE;

	// DO NOT create a child partition here!
	// The child partition would bypass encryption entirely.
	// Instead, we just mark this partition as encrypted and let the
	// virtual device driver (encrypted_disk) handle creating the decrypted view
	// when the user provides the passphrase.

	// Mark the partition as encrypted
	partition->flags |= B_PARTITION_IS_DEVICE; // This partition represents a device
	partition->content_type = strdup("Haiku Encrypted Volume");

	// Store the header info in the partition's content_cookie for later use
	partition->content_size = header->data_size;
	partition->content_cookie = header;
	// Note: ownership of header is transferred to partition->content_cookie

	TRACE("scan_partition: marked partition as encrypted, size=%" B_PRIdOFF "\n",
		header->data_size);
	TRACE("scan_partition: NO child partition created - waiting for encrypted_disk driver\n");

	// Try to load the encrypted_disk driver module
	// This ensures the driver is available when users try to unlock partitions
	TRACE("scan_partition: attempting to load encrypted_disk driver module\n");
	module_info* module;
	status_t loadStatus = get_module("drivers/disk/virtual/encrypted_disk/driver_v2", &module);
	if (loadStatus == B_OK) {
		TRACE("scan_partition: encrypted_disk driver module loaded successfully\n");
		put_module("drivers/disk/virtual/encrypted_disk/driver_v2");
	} else {
		TRACE_ERROR("scan_partition: failed to load encrypted_disk driver: %s\n",
			strerror(loadStatus));
	}

	return B_OK;
}


static void
encrypted_free_identify_partition_cookie(partition_data* partition, void* _cookie)
{
	TRACE("free_identify_partition_cookie\n");
	// Cookie might be NULL if we transferred ownership to partition->content_cookie
	if (_cookie != NULL)
		free(_cookie);
}


static status_t
encrypted_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			TRACE("module init - encrypted partitioning system loaded\n");
			return B_OK;
		case B_MODULE_UNINIT:
			TRACE("module uninit - encrypted partitioning system unloaded\n");
			return B_OK;
		default:
			TRACE("unknown module operation: %d\n", op);
			return B_ERROR;
	}
}


static partition_module_info sEncryptedPartitioningModule = {
	{"partitioning_systems/encrypted/v1", 0, encrypted_std_ops},

	"encrypted", // short_name
	"Encrypted Disk", // pretty_name
	0, // flags

	// scanning
	encrypted_identify_partition, encrypted_scan_partition,
	encrypted_free_identify_partition_cookie,

	// querying
	NULL, // get_supported_operations
	NULL, // get_supported_child_operations
	NULL, // supports_initializing_child
	NULL, // is_sub_system_for

	NULL, // validate_resize
	NULL, // validate_resize_child
	NULL, // validate_move
	NULL, // validate_move_child
	NULL, // validate_set_content_name
	NULL, // validate_set_content_parameters
	NULL, // validate_initialize

	// shadow partition modification
	NULL, // shadow_changed

	// writing
	NULL, // defragment
	NULL, // repair
	NULL, // resize
	NULL, // resize_child
	NULL, // move
	NULL, // move_child
	NULL, // set_content_name
	NULL, // set_content_parameters
	NULL, // initialize
	NULL // uninitialize
};

extern "C"
{
	module_info* modules[] = {(module_info*)&sEncryptedPartitioningModule, NULL};
}
