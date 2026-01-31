"""
PlatformIO Pre-Script: Set User Data Directory
Used by hub_esp32_userfs environment for user JSON files
"""
Import("env")
import os

project_dir = env["PROJECT_DIR"]

# User data directory: JSON config files
# Located at src/hub/data_user
user_data_dir = os.path.join(project_dir, "src", "hub", "data_user")

env.Replace(PROJECT_DATA_DIR=user_data_dir)
env.Replace(PROJECTDATA_DIR=user_data_dir)
env.Replace(DATA_DIR=user_data_dir)

print("[set_user_data_dir] Using user data_dir: {}".format(user_data_dir))
