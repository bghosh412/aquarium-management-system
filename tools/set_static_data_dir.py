"""
PlatformIO Pre-Script: Set Static Data Directory
Used by hub_esp32_staticfs environment for UI + hub_config files
"""
Import("env")
import os

project_dir = env["PROJECT_DIR"]

# Static data directory: UI files + hub_config.txt
# Located at src/hub/data/static (we'll create this structure)
static_data_dir = os.path.join(project_dir, "src", "hub", "data_static")

env.Replace(PROJECT_DATA_DIR=static_data_dir)
env.Replace(PROJECTDATA_DIR=static_data_dir)
env.Replace(DATA_DIR=static_data_dir)

print("[set_static_data_dir] Using static data_dir: {}".format(static_data_dir))
