"""
PlatformIO Post-Script: Build Static Filesystem
Configures mklittlefs to target the static_fs partition
"""
Import("env")

# Get partition info for static_fs from CSV
# static_fs is at offset 0x610000, size 0x700000 (7MB)
STATIC_FS_OFFSET = 0x610000
STATIC_FS_SIZE = 0x700000  # 7MB

# Update board configuration for filesystem
board_config = env.BoardConfig()

# Set the filesystem partition name and offset
board_config.update("build.partitions", "partitions/dual_littlefs_16MB.csv")

# Override the spiffs/littlefs start address in board config
env.Replace(
    MKSPIFFSTOOL="mklittlefs",
)

# Set flash offset for upload
env["UPLOAD_FLAGS"] = [
    "--flash_mode", "dio",
    "--flash_size", "16MB",
    "--flash_freq", "80m"
]

# Override the upload command to use correct offset
env.Replace(
    UPLOADERFLAGS=[
        "--chip", "esp32s3",
        "--port", "$UPLOAD_PORT",
        "--baud", "$UPLOAD_SPEED",
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "-z",
        hex(STATIC_FS_OFFSET),
    ]
)

# Set filesystem size for mklittlefs
env.Append(
    MKFSOPTS=[
        "-s", str(STATIC_FS_SIZE),
        "-p", "256",  # page size
        "-b", "4096",  # block size
    ]
)

print("[build_static_fs] Configured for static_fs partition")
print("  Offset: 0x{:X}".format(STATIC_FS_OFFSET))
print("  Size: {} bytes ({} MB)".format(STATIC_FS_SIZE, STATIC_FS_SIZE // (1024*1024)))
