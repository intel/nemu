package main

import (
	"context"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"os/exec"
	"os/user"
	"path"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/intel/govmm/qemu"
	"golang.org/x/crypto/ssh"
)

var sshPort uint16 = 2222
var sshPortMutex = sync.Mutex{}

var cancelTimeout = 4 * time.Minute

func getNemuPath(t *testing.T) string {
	u, err := user.Current()
	if err != nil {
		t.Errorf("Error getting current user: %v", err)
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

func (q *qemuTest) getSourceDiskImage(t *testing.T) string {
	u, err := user.Current()
	if err != nil {
		t.Errorf("Error getting current user: %v", err)
		os.Exit(1)
	}

	diskImage := q.diskImage
	if diskImage == "" {
		diskImage = clearDiskImage
	}
	return path.Join(u.HomeDir, "workloads", diskImage)

}

// Expects to be run the nats directory so goes up a level to find the cloud-init data
func getCloudInitPath(t *testing.T) string {
	cwd, err := os.Getwd()
	if err != nil {
		t.Errorf("Error getting current directory: %v", err)
	}
	tmp := strings.Split(cwd, "/")
	tmp = append(tmp[:len(tmp)-1], "cloud-init")
	return strings.Join(tmp, "/")
}

func (q *qemuTest) createCloudInitImage(t *testing.T) string {
	cloudInitImageFile, err := ioutil.TempFile("", "nemu-test")
	if err != nil {
		t.Fatalf("Error creating temporary file for cloud init image: %v", err)
	}
	cloudInitImagePath := cloudInitImageFile.Name()
	cloudInitImageFile.Truncate(2 * 1024 * 1024) // 2 MiB
	cloudInitImageFile.Close()

	cloudInit := q.cloudInit
	if cloudInit == "" {
		cloudInit = cloudInitClear
	}

	if cloudInit == cloudInitClear {
		cmd := exec.Command("mkfs.vfat", "-n", "config-2", cloudInitImagePath)
		output, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("Error creating filesystem for cloud init image: %v: %s", err, string(output))
		}

		cmd = exec.Command("mcopy", "-oi", cloudInitImagePath, "-s", fmt.Sprintf("%s/clear/openstack", getCloudInitPath(t)), "::")
		output, err = cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("Error copying files for cloud init image: %v: %s", err, string(output))
		}
	} else if cloudInit == cloudInitUbuntu {
		cmd := exec.Command("mkfs.vfat", "-n", "cidata", cloudInitImagePath)
		output, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("Error creating filesystem for cloud init image: %v: %s", err, string(output))
		}

		cmd = exec.Command("mcopy", "-oi", cloudInitImagePath, fmt.Sprintf("%s/ubuntu/user-data", getCloudInitPath(t)), "::")
		output, err = cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("Error copying files for cloud init image: %v: %s", err, string(output))
		}

		cmd = exec.Command("mcopy", "-oi", cloudInitImagePath, fmt.Sprintf("%s/ubuntu/meta-data", getCloudInitPath(t)), "::")
		output, err = cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("Error copying files for cloud init image: %v: %s", err, string(output))
		}
	} else {
		t.Fatal("Unexpected cloud-init type")
	}

	return cloudInitImagePath
}

func (q *qemuTest) createPrimaryDiskImage(t *testing.T) string {
	primaryDiskImageFile, err := ioutil.TempFile("", "nemu-test")
	if err != nil {
		t.Fatalf("Error creating temporary file for primary disk image: %v", err)
	}
	primaryDiskImagePath := primaryDiskImageFile.Name()

	f, err := os.Open(q.getSourceDiskImage(t))
	if err != nil {
		t.Fatalf("Error opening source disk image: %v", err)
	}

	_, err = io.Copy(primaryDiskImageFile, f)
	if err != nil {
		t.Fatalf("Error copying source disk image: %v", err)
	}
	return primaryDiskImagePath
}

func (q *qemuTest) runCommandBySSH(command string, t *testing.T) string {
	config := &ssh.ClientConfig{
		User: "nemu",
		Auth: []ssh.AuthMethod{
			ssh.Password("nemu123"),
		},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
	}

	var client *ssh.Client
	var err error
	for i := 1; i <= 5; i++ {
		client, err = ssh.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", q.sshPort), config)
		if err != nil {
			if i == 5 {
				t.Errorf("Failed to dial: %v", err)
				return ""
			}
		} else {
			break
		}
		time.Sleep(10 * time.Second)
	}

	session, err := client.NewSession()
	if err != nil {
		t.Errorf("Failed to create session: %v", err)
		return ""
	}
	defer session.Close()

	output, err := session.CombinedOutput(command)
	if err != nil {
		t.Logf("SSH command generated error: %v", err)
	}

	return string(output)
}

type simpleLogger struct {
	t *testing.T
}

func (l simpleLogger) V(level int32) bool {
	return false
}

func (l simpleLogger) Infof(format string, v ...interface{}) {
	l.t.Logf(format, v)
}

func (l simpleLogger) Warningf(format string, v ...interface{}) {
	l.t.Logf(format, v)
}

func (l simpleLogger) Errorf(format string, v ...interface{}) {
	l.t.Logf(format, v)
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

	sshPortMutex.Lock()
	q.sshPort = sshPort
	sshPort++
	sshPortMutex.Unlock()

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
		"-device", "virtio-serial-pci,id=virtio-serial0",
		"-device", "virtconsole,chardev=charconsole0,id=console0",
		"-chardev", fmt.Sprintf("file,id=charconsole0,path=%s,server,nowait", virtConsoleLogFile.Name()),
		"-device", "virtio-rng-pci,rng=rng0",
		"-object", "rng-random,filename=/dev/random,id=rng0",
		"-device", "virtio-balloon-pci",
		"-object", "cryptodev-backend-builtin,id=cryptodev0",
		"-device", "virtio-crypto-pci,id=crypto0,cryptodev=cryptodev0",
	}

	if q.machine == "virt" {
		q.params = append(q.params,
			"-device", "sysbus-debugcon,iobase=0x402,chardev=debugcon",
			"-chardev", fmt.Sprintf("file,path=%s,id=debugcon", sysbusDebugLogFile.Name()),
			"-device", "sysbus-debugcon,iobase=0x3f8,chardev=serialcon",
			"-chardev", fmt.Sprintf("file,path=%s,id=serialcon", serialOutputLogFile.Name()))
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

	_, err = qemu.LaunchCustomQemu(ctx, getNemuPath(t), q.params, fds, nil, simpleLogger{t: t})
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

type cloudInitType string

const (
	cloudInitClear  cloudInitType = "clear"
	cloudInitUbuntu cloudInitType = "ubuntu"
)

const (
	clearDiskImage  = "clear-24950-cloud.img"
	ubuntuDiskImage = "xenial-server-cloudimg-amd64-uefi1.img"
)

type qemuTest struct {
	qmp       *qemu.QMP
	params    []string
	doneCh    chan interface{}
	machine   string
	sshPort   uint16
	cloudInit cloudInitType
	diskImage string
}

func (q *qemuTest) startQemu(ctx context.Context, t *testing.T) error {
	monitorSocketCh := make(chan string, 1)

	q.doneCh = make(chan interface{})
	go func() {
		q.launchQemu(ctx, monitorSocketCh, t)
		close(q.doneCh)
	}()

	time.Sleep(time.Second * 5)
	config := qemu.QMPConfig{
		Logger: simpleLogger{t: t},
	}
	disconnectedCh := make(chan struct{})
	qmp, qmpVersion, err := qemu.QMPStart(ctx, <-monitorSocketCh, config, disconnectedCh)
	t.Logf("\nQMP version: %v\n", *qmpVersion)
	if err != nil {
		return err
	}
	q.qmp = qmp

	err = q.qmp.ExecuteQMPCapabilities(ctx)
	if err != nil {
		return err
	}

	return nil
}

var machines = []string{"pc", "q35", "virt"}

type distro struct {
	image     string
	cloudInit cloudInitType
}

var distros = []distro{
	{
		image:     ubuntuDiskImage,
		cloudInit: cloudInitUbuntu,
	},
	{
		image:     clearDiskImage,
		cloudInit: cloudInitClear,
	},
}

func TestShutdown(t *testing.T) {
	t.Parallel()
	for _, m := range machines {
		t.Logf("Testing machine: %s", m)

		for _, d := range distros {
			t.Logf("Testing with disk image: %s", d.image)
			q := qemuTest{
				machine:   m,
				diskImage: d.image,
				cloudInit: d.cloudInit,
			}
			ctx, cancelFunc := context.WithTimeout(context.Background(), cancelTimeout)
			err := q.startQemu(ctx, t)
			if err != nil {
				cancelFunc()
				<-q.doneCh
				t.Fatalf("Error starting qemu: %v", err)
			}

			time.Sleep(time.Second * 15)
			q.runCommandBySSH("sudo shutdown -h now", t)

			<-q.doneCh
			cancelFunc()
		}
	}
}

func TestReboot(t *testing.T) {
	t.Parallel()
	for _, m := range machines {
		t.Logf("Testing machine: %s", m)
		for _, d := range distros {
			t.Logf("Testing with disk image: %s", d.image)
			q := qemuTest{
				machine:   m,
				diskImage: d.image,
				cloudInit: d.cloudInit,
			}
			ctx, cancelFunc := context.WithTimeout(context.Background(), cancelTimeout)
			err := q.startQemu(ctx, t)
			if err != nil {
				cancelFunc()
				<-q.doneCh
				t.Fatalf("Error starting qemu: %v", err)
			}

			time.Sleep(time.Second * 15)
			q.runCommandBySSH("sudo reboot", t)
			time.Sleep(time.Second * 15)
			q.runCommandBySSH("sudo shutdown -h now", t)

			<-q.doneCh
			cancelFunc()
		}
	}
}

func TestCheckAcpiTables(t *testing.T) {
	t.Parallel()
	tableCounts := map[string]int{
		"pc":   8,
		"q35":  9,
		"virt": 8,
	}
	for _, m := range machines {
		t.Logf("Testing machine: %s", m)
		q := qemuTest{
			machine: m,
		}
		ctx, cancelFunc := context.WithTimeout(context.Background(), cancelTimeout)
		err := q.startQemu(ctx, t)
		if err != nil {
			cancelFunc()
			<-q.doneCh
			t.Fatalf("Error starting qemu: %v", err)
		}

		time.Sleep(time.Second * 15)
		dmesgOutput := q.runCommandBySSH("sudo dmesg", t)

		r := regexp.MustCompile("ACPI:.*BOCHS.*")
		matches := r.FindAllStringIndex(dmesgOutput, -1)

		if len(matches) != tableCounts[m] {
			t.Errorf("Unexpected number of ACPI tables from QEMU: %v", len(matches))
			t.Logf("\n\n==== dmesg output: ===\n\n")
			t.Log(dmesgOutput)
		}

		time.Sleep(time.Second * 15)
		q.runCommandBySSH("sudo shutdown -h now", t)

		<-q.doneCh
		cancelFunc()
	}
}

func TestQMPQuit(t *testing.T) {
	t.Parallel()
	for _, m := range machines {
		t.Logf("Testing machine: %s", m)
		q := qemuTest{
			machine: m,
		}
		ctx, cancelFunc := context.WithTimeout(context.Background(), cancelTimeout)
		err := q.startQemu(ctx, t)
		if err != nil {
			cancelFunc()
			<-q.doneCh
			t.Fatalf("Error starting qemu: %v", err)
		}

		time.Sleep(time.Second * 15)
		err = q.qmp.ExecuteQuit(ctx)
		if err != nil {
			t.Errorf("Error quiting via QMP: %v", err)
		}

		<-q.doneCh
		cancelFunc()
	}
}

func (q *qemuTest) getTotalMemory(t *testing.T) int {
	m := q.runCommandBySSH(`cat /proc/meminfo  | grep MemTotal | sed "s/.*: *\([0-9]*\) kB/\1/"`, t)
	mem, err := strconv.Atoi(strings.TrimSpace(m))
	if err != nil {
		t.Errorf("Error converting memory value to int: %v", err)
	}
	return mem
}

func TestMemoryHotplug(t *testing.T) {
	t.Parallel()
	for _, m := range machines {
		t.Logf("Testing machine: %s", m)

		q := qemuTest{
			machine: m,
		}
		ctx, cancelFunc := context.WithTimeout(context.Background(), cancelTimeout)
		err := q.startQemu(ctx, t)
		if err != nil {
			cancelFunc()
			<-q.doneCh
			t.Fatalf("Error starting qemu: %v", err)
		}

		addedMemoryMiB := 512
		beforeMem := q.getTotalMemory(t)
		err = q.qmp.ExecHotplugMemory(ctx, "memory-backend-ram", "memslot1", "", addedMemoryMiB)
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

		<-q.doneCh
		cancelFunc()
	}
}

func TestCPUHotplug(t *testing.T) {
	t.Parallel()
	for _, m := range machines {
		t.Logf("Testing machine: %s", m)

		q := qemuTest{
			machine: m,
		}
		ctx, cancelFunc := context.WithTimeout(context.Background(), cancelTimeout)
		err := q.startQemu(ctx, t)
		if err != nil {
			cancelFunc()
			<-q.doneCh
			t.Fatalf("Error starting qemu: %v", err)
		}

		cpusOnlineBefore := strings.TrimSpace(q.runCommandBySSH("cat /sys/devices/system/cpu/online", t))
		if cpusOnlineBefore != "0-1" {
			t.Errorf("Unexpected online cpus: %s", cpusOnlineBefore)
		}

		err = q.qmp.ExecuteCPUDeviceAdd(ctx, "host-x86_64-cpu", "core2", "2", "0", "0")
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

		<-q.doneCh
		cancelFunc()
	}
}
