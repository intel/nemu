package main

import (
	"context"
	"fmt"
	"io/ioutil"
	"os"
	"os/user"
	"path"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/intel/govmm/qemu"
)

func getNemuPath() string {
	u, err := user.Current()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error getting current user: %v", err)
		os.Exit(1)
	}

	return path.Join(u.HomeDir, "build-x86_64", "x86_64-softmmu", "qemu-system-x86_64")
}

func getBiosPath(t *testing.T) string {
	u, err := user.Current()
	if err != nil {
		t.Errorf("Error getting current user: %v", err)
		os.Exit(1)
	}

	return path.Join(u.HomeDir, "workloads", "OVMF.fd")
}

func (q *qemuTest) launchQemu(ctx context.Context, monitorSocketCh chan string, t *testing.T) {
	sysbusDebugLogFile, err := ioutil.TempFile("", "nemu-test")
	if err != nil {
		t.Fatalf("Error creating temporary file for sysbus debug log: %v", err)
	}
	defer os.Remove(sysbusDebugLogFile.Name())

	serialOutputLogFile, err := ioutil.TempFile("", "nemu-test")
	if err != nil {
		t.Fatalf("Error creating temporary for serial output: %v", err)
	}
	defer os.Remove(serialOutputLogFile.Name())

	virtConsoleLogFile, err := ioutil.TempFile("", "nemu-test")
	if err != nil {
		t.Fatalf("Error creating temporary for console output: %v", err)
	}
	defer os.Remove(virtConsoleLogFile.Name())

	primaryDiskImagePath := q.createPrimaryDiskImage(t)
	defer os.Remove(primaryDiskImagePath)
	cloudInitImagePath := q.createCloudInitImage(t)
	defer os.Remove(cloudInitImagePath)

	q.sshPort = allocateSSHPort()

	q.params = []string{
		"-machine", fmt.Sprintf("%s,accel=kvm,kernel_irqchip,nvdimm", q.machine),
		"-bios", getBiosPath(t),
		"-smp", "2,cores=1,threads=1,sockets=2,maxcpus=32",
		"-m", "512,slots=4,maxmem=16384M",
		"-cpu", "host",
		"-nographic",
		"-no-user-config",
		"-nodefaults",
		"-drive", fmt.Sprintf("file=%s,if=none,id=drive-virtio-disk0,format=qcow2", primaryDiskImagePath),
		"-device", "virtio-blk-pci,scsi=off,drive=drive-virtio-disk0,id=virtio-disk0",
		"-device", "virtio-blk-pci,drive=cloud",
		"-drive", fmt.Sprintf("if=none,id=cloud,file=%s,format=raw", cloudInitImagePath),
		"-netdev", fmt.Sprintf("user,id=mynet0,hostfwd=tcp::%d-:22,hostname=nemuvm", q.sshPort),
		"-device", "virtio-net-pci,netdev=mynet0",
		"-device", "virtio-rng-pci,rng=rng0",
		"-object", "rng-random,filename=/dev/random,id=rng0",
		"-device", "virtio-balloon-pci",
		"-object", "cryptodev-backend-builtin,id=cryptodev0",
		"-device", "virtio-crypto-pci,id=crypto0,cryptodev=cryptodev0",
	}

	if q.machine == "virt" {
		q.params = append(q.params,
			"-device", "sysbus-debugcon,iobase=0x402,chardev=debugcon",
			"-chardev", fmt.Sprintf("file,path=%s,id=debugcon", sysbusDebugLogFile.Name()))
	} else {
		q.params = append(q.params,
			"-device", "isa-debugcon,iobase=0x402,chardev=debugcon",
			"-chardev", fmt.Sprintf("file,path=%s,id=debugcon", sysbusDebugLogFile.Name()),
			"-device", "isa-debugcon,iobase=0x3f8,chardev=serialcon",
			"-chardev", fmt.Sprintf("file,path=%s,id=serialcon", serialOutputLogFile.Name()))
	}

	if monitorSocketCh != nil {
		monitorSocketFile, err := ioutil.TempFile("", "nemu-test")
		if err != nil {
			t.Fatalf("Error creating temporary file for QMP socket: %v", err)
		}
		defer os.Remove(monitorSocketFile.Name())
		q.params = append(q.params, "-qmp", fmt.Sprintf("unix:%s,server,nowait", monitorSocketFile.Name()))
		monitorSocketFile.Close()
		monitorSocketCh <- monitorSocketFile.Name()
		close(monitorSocketCh)
	}

	fds := []*os.File{}

	_, err = qemu.LaunchCustomQemu(ctx, *nemuBinaryPath, q.params, fds, nil, simpleLogger{t: t})
	if err != nil {
		t.Errorf("Error launching QEMU: %v", err)

		t.Logf("\n\n==== sysbus (OVMF) debug output: ===\n\n")
		data, err := ioutil.ReadAll(sysbusDebugLogFile)
		if err != nil {
			t.Errorf("Error reading sysbus debug output: %v", err)
		}
		t.Log(string(data))

		t.Logf("\n\n==== serial console output: ===\n\n")
		data, err = ioutil.ReadAll(serialOutputLogFile)
		if err != nil {
			t.Errorf("Error reading serial console output: %v", err)
		}
		t.Log(string(data))

		t.Logf("\n\n==== virt console output: ===\n\n")
		data, err = ioutil.ReadAll(virtConsoleLogFile)
		if err != nil {
			t.Errorf("Error reading virt console output: %v", err)
		}
		t.Log(string(data))
	}
}

func testCheckAcpiTables(ctx context.Context, q *qemuTest, t *testing.T) {
	tableCounts := map[string]int{
		"pc":   8,
		"q35":  9,
		"virt": 8,
	}
	time.Sleep(time.Second * 15)
	dmesgOutput := q.runCommandBySSH("sudo dmesg", t)

	r := regexp.MustCompile("ACPI:.*BOCHS.*")
	matches := r.FindAllStringIndex(dmesgOutput, -1)

	if len(matches) != tableCounts[q.machine] {
		t.Errorf("Unexpected number of ACPI tables from QEMU: %v", len(matches))
		t.Logf("\n\n==== dmesg output: ===\n\n")
		t.Log(dmesgOutput)
	}

	time.Sleep(time.Second * 15)
	q.runCommandBySSH("sudo shutdown -h now", t)
}

func (q *qemuTest) getTotalMemory(t *testing.T) int {
	m := q.runCommandBySSH(`cat /proc/meminfo  | grep MemTotal | sed "s/.*: *\([0-9]*\) kB/\1/"`, t)
	mem, err := strconv.Atoi(strings.TrimSpace(m))
	if err != nil {
		t.Errorf("Error converting memory value to int: %v", err)
	}
	return mem
}

func testMemoryHotplug(ctx context.Context, q *qemuTest, t *testing.T) {
	addedMemoryMiB := 512
	beforeMem := q.getTotalMemory(t)
	err := q.qmp.ExecHotplugMemory(ctx, "memory-backend-ram", "memslot1", "", addedMemoryMiB)
	if err != nil {
		t.Errorf("Error adding memory to guest: %v", err)
	}
	afterMem := q.getTotalMemory(t)

	expectedMemoryKiB := beforeMem + (addedMemoryMiB * 1024)
	if afterMem != expectedMemoryKiB {
		t.Errorf("Hotplugging memory did not result in expected values: before: %v after: %v expected: %v",
			beforeMem, afterMem, expectedMemoryKiB)
	}

	time.Sleep(time.Second * 15)
	err = q.qmp.ExecuteQuit(ctx)
	if err != nil {
		t.Errorf("Error quiting via QMP: %v", err)
	}

}

func testCPUHotplug(ctx context.Context, q *qemuTest, t *testing.T) {
	cpusOnlineBefore := strings.TrimSpace(q.runCommandBySSH("cat /sys/devices/system/cpu/online", t))
	if cpusOnlineBefore != "0-1" {
		t.Errorf("Unexpected online cpus: %s", cpusOnlineBefore)
	}

	err := q.qmp.ExecuteCPUDeviceAdd(ctx, "host-x86_64-cpu", "core2", "2", "0", "0")
	if err != nil {
		t.Errorf("Error hotplugging CPU: %v", err)
	}

	time.Sleep(time.Second * 15)
	q.runCommandBySSH(`sudo sh -c "echo 1 > /sys/devices/system/cpu/cpu2/online"`, t)
	time.Sleep(time.Second * 15)

	cpusOnlineAfter := strings.TrimSpace(q.runCommandBySSH("cat /sys/devices/system/cpu/online", t))
	if cpusOnlineAfter != "0-2" {
		t.Errorf("Unexpected online cpus: %s", cpusOnlineAfter)
	}

	time.Sleep(time.Second * 15)
	err = q.qmp.ExecuteQuit(ctx)
	if err != nil {
		t.Errorf("Error quiting via QMP: %v", err)
	}
}

var machines = []string{"pc", "q35", "virt"}

const (
	clearDiskImage  = "clear-24740-cloud.img"
	xenialDiskImage = "xenial-server-cloudimg-amd64-uefi1.img"
)

var allDistros = []distro{
	{
		name:      "xenial",
		image:     xenialDiskImage,
		cloudInit: cloudInitUbuntu,
	},
	{
		name:      "clear",
		image:     clearDiskImage,
		cloudInit: cloudInitClear,
	},
}

var clearLinuxOnly = []distro{
	{
		name:      "clear",
		image:     clearDiskImage,
		cloudInit: cloudInitClear,
	},
}

var allMachines = []string{"pc", "q35", "virt"}

var tests = []testConfig{
	{
		name:     "Shutdown",
		testFunc: testShutdown,
		distros:  allDistros,
		machines: allMachines,
	},
	{
		name:     "Reboot",
		testFunc: testReboot,
		distros:  allDistros,
		machines: allMachines,
	},
	{
		name:     "CheckACPITables",
		testFunc: testCheckAcpiTables,
		distros:  allDistros,
		machines: allMachines,
	},
	{
		name:     "QMPQuit",
		testFunc: testQMPQuit,
		distros:  allDistros,
		machines: allMachines,
	},
	{
		name:     "CPUHotplug",
		testFunc: testCPUHotplug,
		distros:  clearLinuxOnly,
		machines: allMachines,
	},
	{
		name:     "MemoryHotplug",
		testFunc: testMemoryHotplug,
		distros:  clearLinuxOnly,
		machines: allMachines,
	},
}
