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
	"strings"
	"testing"
	"time"

	"github.com/intel/govmm/qemu"
	"golang.org/x/crypto/ssh"
)

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

func getSourceDiskImage(t *testing.T) string {
	u, err := user.Current()
	if err != nil {
		t.Errorf("Error getting current user: %v", err)
		os.Exit(1)
	}

	return path.Join(u.HomeDir, "workloads", "clear-24550-cloud.img")

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

func createCloudInitImage(t *testing.T) string {
	cloudInitImageFile, err := ioutil.TempFile("", "nemu-test")
	if err != nil {
		t.Fatalf("Error creating temporary file for cloud init image: %v", err)
	}
	cloudInitImagePath := cloudInitImageFile.Name()
	cloudInitImageFile.Truncate(2 * 1024 * 1024) // 2 MiB
	cloudInitImageFile.Close()

	cmd := exec.Command("mkfs.vfat", "-n", "config-2", cloudInitImagePath)
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("Error creating fileystem for cloud init image: %v: %s", err, string(output))
	}

	cmd = exec.Command("mcopy", "-oi", cloudInitImagePath, "-s", fmt.Sprintf("%s/openstack", getCloudInitPath(t)), "::")
	output, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("Error copying files for cloud init image: %v: %s", err, string(output))
	}

	return cloudInitImagePath
}

func createPrimaryDiskImage(t *testing.T) string {
	primaryDiskImageFile, err := ioutil.TempFile("", "nemu-test")
	if err != nil {
		t.Fatalf("Error creating temporary file for primary disk image: %v", err)
	}
	primaryDiskImagePath := primaryDiskImageFile.Name()

	f, err := os.Open(getSourceDiskImage(t))
	if err != nil {
		t.Fatalf("Error opening source disk image: %v", err)
	}

	_, err = io.Copy(primaryDiskImageFile, f)
	if err != nil {
		t.Fatalf("Error copying source disk image: %v", err)
	}
	return primaryDiskImagePath
}

func runCommandBySSH(command string, t *testing.T) string {
	config := &ssh.ClientConfig{
		User: "nemu",
		Auth: []ssh.AuthMethod{
			ssh.Password("nemu123"),
		},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
	}
	client, err := ssh.Dial("tcp", "127.0.0.1:2222", config)
	if err != nil {
		t.Errorf("Failed to dial: %v", err)
	}

	session, err := client.NewSession()
	if err != nil {
		t.Errorf("Failed to create session: %v", err)
	}
	defer session.Close()

	output, err := session.CombinedOutput(command)
	if err != nil {
		t.Logf("SSH command generated error: %v", err)
	}

	return string(output)
}

type simpleLogger struct{}

func (l simpleLogger) V(level int32) bool {
	return false
}

func (l simpleLogger) Infof(format string, v ...interface{}) {
	fmt.Fprintf(os.Stderr, format, v)
}

func (l simpleLogger) Warningf(format string, v ...interface{}) {
	fmt.Fprintf(os.Stderr, format, v)
}

func (l simpleLogger) Errorf(format string, v ...interface{}) {
	fmt.Fprintf(os.Stderr, format, v)
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

	primaryDiskImagePath := createPrimaryDiskImage(t)
	defer os.Remove(primaryDiskImagePath)
	cloudInitImagePath := createCloudInitImage(t)
	defer os.Remove(cloudInitImagePath)

	q.params = []string{
		"-machine", "virt,accel=kvm,kernel_irqchip,nvdimm",
		"-bios", getBiosPath(t),
		"-smp", "2,cores=1,threads=1,sockets=2,maxcpus=32",
		"-m", "512,slots=4,maxmem=16384M",
		"-cpu", "host",
		"-nographic",
		"-no-user-config",
		"-nodefaults",
		"-drive", fmt.Sprintf("file=%s,if=none,id=drive-virtio-disk0,format=qcow2", primaryDiskImagePath),
		"-device", "virtio-blk-pci,scsi=off,drive=drive-virtio-disk0,id=virtio-disk0",
		"-device", "sysbus-debugcon,iobase=0x402,chardev=debugcon",
		"-chardev", fmt.Sprintf("file,path=%s,id=debugcon", sysbusDebugLogFile.Name()),
		"-device", "sysbus-debugcon,iobase=0x3f8,chardev=serialcon",
		"-chardev", fmt.Sprintf("file,path=%s,id=serialcon", serialOutputLogFile.Name()),
		"-device", "virtio-blk-pci,drive=cloud",
		"-drive", fmt.Sprintf("if=none,id=cloud,file=%s,format=raw", cloudInitImagePath),
		"-netdev", "user,id=mynet0,hostfwd=tcp::2222-:22,hostname=nemuvm",
		"-device", "virtio-net-pci,netdev=mynet0",
		"-device", "virtio-serial-pci,id=virtio-serial0",
		"-device", "virtconsole,chardev=charconsole0,id=console0",
		"-chardev", "socket,id=charconsole0,path=console.sock,server,nowait",
		"-device", "virtio-rng-pci,rng=rng0",
		"-object", "rng-random,filename=/dev/random,id=rng0",
		"-device", "virtio-balloon-pci",
		"-object", "cryptodev-backend-builtin,id=cryptodev0",
		"-device", "virtio-crypto-pci,id=crypto0,cryptodev=cryptodev0",
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

	_, err = qemu.LaunchCustomQemu(ctx, getNemuPath(t), q.params, fds, nil, simpleLogger{})
	if err != nil {
		t.Errorf("Error launching QEMU: %v", err)

		fmt.Fprintf(os.Stderr, "\n\n==== sysbus (OVMF) debug output: ===\n\n")
		data, err := ioutil.ReadAll(sysbusDebugLogFile)
		if err != nil {
			t.Errorf("Error reading sysbus debug output: %v", err)
		}
		fmt.Fprintln(os.Stderr, string(data))

		fmt.Fprintf(os.Stderr, "\n\n==== serial console output: ===\n\n")
		data, err = ioutil.ReadAll(serialOutputLogFile)
		if err != nil {
			t.Errorf("Error reading serial console output: %v", err)
		}
		fmt.Fprintln(os.Stderr, string(data))
	}
}

type qemuTest struct {
	qmp    *qemu.QMP
	params []string
	doneCh chan interface{}
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
		Logger: simpleLogger{},
	}
	disconnectedCh := make(chan struct{})
	qmp, qmpVersion, err := qemu.QMPStart(ctx, <-monitorSocketCh, config, disconnectedCh)
	fmt.Fprintf(os.Stderr, "\nQMP version: %v\n", *qmpVersion)
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

func TestShutdown(t *testing.T) {
	q := qemuTest{}
	ctx, cancelFunc := context.WithTimeout(context.Background(), 120*time.Second)
	err := q.startQemu(ctx, t)
	if err != nil {
		cancelFunc()
		<-q.doneCh
		t.Fatalf("Error starting qemu: %v", err)
	}

	time.Sleep(time.Second * 15)
	runCommandBySSH("sudo shutdown -h now", t)

	<-q.doneCh
	cancelFunc()
}

func TestReboot(t *testing.T) {
	q := qemuTest{}
	ctx, cancelFunc := context.WithTimeout(context.Background(), 120*time.Second)
	err := q.startQemu(ctx, t)
	if err != nil {
		cancelFunc()
		<-q.doneCh
		t.Fatalf("Error starting qemu: %v", err)
	}

	time.Sleep(time.Second * 15)
	runCommandBySSH("sudo reboot", t)
	time.Sleep(time.Second * 15)
	runCommandBySSH("sudo shutdown -h now", t)

	<-q.doneCh
	cancelFunc()
}

func TestCheckDmesg(t *testing.T) {
	q := qemuTest{}
	ctx, cancelFunc := context.WithTimeout(context.Background(), 120*time.Second)
	err := q.startQemu(ctx, t)
	if err != nil {
		cancelFunc()
		<-q.doneCh
		t.Fatalf("Error starting qemu: %v", err)
	}

	time.Sleep(time.Second * 15)
	dmesgOutput := runCommandBySSH("sudo dmesg", t)

	r := regexp.MustCompile("ACPI:.*BOCHS.*")
	matches := r.FindAllStringIndex(dmesgOutput, -1)

	if len(matches) != 8 {
		t.Errorf("Unexpected number of ACPI tables from QEMU: %v", len(matches))
		fmt.Fprintf(os.Stderr, "\n\n==== dmesg output: ===\n\n")
		fmt.Fprintln(os.Stderr, dmesgOutput)
	}

	time.Sleep(time.Second * 15)
	runCommandBySSH("sudo shutdown -h now", t)

	<-q.doneCh
	cancelFunc()
}
