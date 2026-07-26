Import("env")

# 新增 upload_all 目标: 同时烧录固件和 LittleFS 文件系统
env.AddCustomTarget(
    "upload_all",
    None,
    [
        "pio run --target upload",
        "pio run --target uploadfs"
    ],
    "Upload firmware AND LittleFS image"
)
