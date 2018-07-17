#!/usr/bin/env python3
import sys
import re
import os


# Load configuration file into map
config_f = open(sys.argv[1])
configs = {}
for l in config_f.readlines():
        (k,v) = l.split("=")
        configs[k] = v.strip()

# When config entries depend on another resolve them
for k, v in configs.copy().items():
        m = re.search('\$\((CONFIG_\w*)\)', v)
        if m:
                if m.group(1) in configs:
                        configs[k] = configs[m.group(1)]
                else:
                        del configs[k]
        else:
                continue


f = open(sys.argv[2])
ll = f.readlines()
ff = open(sys.argv[2] + ".tmp", "w+")
ff.truncate()

# skip is used to handle the removal of lines which end in \ for continuation
skip = False
for l in ll:
        if skip:
                if not l.strip().endswith("\\"):
                        skip = False
                continue

        # If a line is a simple config entry..
        m = re.search('\-\$\((CONFIG_\w*)\)', l)
        if m:
                # ... and that is not included in the host configs ...
                if not m.group(1) in configs:
                        # ... skip that line and handle any continuations
                        if l.strip().endswith("\\"):
                                skip = True
                        continue
        ff.writelines([l])
f.close()
ff.close()

os.rename(sys.argv[2] + ".tmp", sys.argv[2])

