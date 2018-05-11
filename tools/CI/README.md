# Build scripts and tools

## First build QEMU

```
SRCDIR=~/nemu ~/nemu/tools/build_x86_64.sh
```

## Simple single image test

### x86-64

```
sudo -E ~/nemu/tools/CI/start_qemu.sh \
        -hypervisor ~/build-x86_64/x86_64-softmmu/qemu-system-x86_64 \
        -imagetype qcow2 \
        -image ~/workloads/fedora.qcow2 \
```

### arm64

```
sudo -E ~/nemu/tools/CI/start_qemu.sh \
        -hypervisor ~/build-aarch64/aarch64-softmmu/qemu-system-aarch64 \
        -platform aarch64 \
        -image ~/workloads/bionic-server-cloudimg-arm64.img \
```


Note: The default user and password is demo/demo

More options
```
sudo -E ~/nemu/tools/CI/start_qemu.sh -hypervisor ~/build-x86_64/x86_64-softmmu/qemu-system-x86_64
```

## Running a full CI

```
sudo -E ~/nemu/tools/CI/minimal_ci.sh -hypervisor ~/build-x86_64/x86_64-softmmu/qemu-system-x86_64
```

Note: The workload images will be downloaded and cached under ~/workloads the first time.
