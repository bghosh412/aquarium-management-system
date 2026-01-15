Import("env")

import os

project_dir = env["PROJECT_DIR"]
pioenv = env.get("PIOENV", "")

node_data_dirs = {
    "node_fish_feeder": os.path.join("src", "nodes", "fish_feeder", "data"),
    "node_co2_regulator": os.path.join("src", "nodes", "co2_regulator", "data"),
    "node_lighting": os.path.join("src", "nodes", "lighting", "data"),
    "node_heater": os.path.join("src", "nodes", "heater", "data"),
    "node_water_quality": os.path.join("src", "nodes", "water_quality", "data"),
    "node_repeater": os.path.join("src", "nodes", "repeater", "data"),
}

if pioenv == "hub_esp32":
    data_dir = os.path.join(project_dir, "src", "hub", "data")
elif pioenv in node_data_dirs:
    data_dir = os.path.join(project_dir, node_data_dirs[pioenv])
else:
    data_dir = os.path.join(project_dir, "data")

# Override project data dir for buildfs
env.Replace(PROJECT_DATA_DIR=data_dir)
env.Replace(PROJECTDATA_DIR=data_dir)
env.Replace(DATA_DIR=data_dir)

print("[set_data_dir] Using data_dir: {}".format(data_dir))
