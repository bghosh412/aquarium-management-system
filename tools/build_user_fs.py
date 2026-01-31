"""
PlatformIO Post-Script: Build User Filesystem
Configures mklittlefs to target the user_fs partition
"""
Import("env")

# Get partition info for user_fs from CSV
# user_fs is at offset 0xD10000, size 0x2E0000 (~3MB)
USER_FS_OFFSET = 0xD10000
USER_FS_SIZE = 0x2E0000  # ~3MB

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
        hex(USER_FS_OFFSET),
    ]
)

# Set filesystem size for mklittlefs
env.Append(
    MKFSOPTS=[
        "-s", str(USER_FS_SIZE),
        "-p", "256",  # page size
        "-b", "4096",  # block size
    ]
)

print("[build_user_fs] Configured for user_fs partition")
print("  Offset: 0x{:X}".format(USER_FS_OFFSET))
print("  Size: {} bytes ({:.1f} MB)".format(USER_FS_SIZE, USER_FS_SIZE / (1024*1024)))
