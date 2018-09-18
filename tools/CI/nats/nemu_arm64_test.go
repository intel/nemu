package main

import (
	"context"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"os/user"
	"path"
	"testing"

	"github.com/intel/govmm/qemu"
)

func createFlashImages(t *testing.T) (string, string) {
	firmwareSourceFile, err := os.Open("/usr/share/qemu-efi/QEMU_EFI.fd")
	if err != nil {
		t.Fatalf("Error creating opening source file for flash image: %v", err)
	}
	defer firmwareSourceFile.Close()

	flashZeroFile, err := ioutil.TempFile("", "nemu-test")
	if err != nil {
		t.Fatalf("Error creating temporary file for flash image: %v", err)
	}
	flashZeroPath := flashZeroFile.Name()
	flashZeroFile.Truncate(64 * 1024 * 1024) // 64 MiB
	flashZeroFile.Seek(0, os.SEEK_SET)
	_, err = io.Copy(flashZeroFile, firmwareSourceFile)
	if err != nil {
		t.Fatalf("Error copying source disk image: %v", err)
	}
	flashZeroFile.Close()

	flashOneFile, err := ioutil.TempFile("", "nemu-test")
	if err != nil {
		t.Fatalf("Error creating temporary file for flash image: %v", err)
	}
	flashOnePath := flashOneFile.Name()
	flashOneFile.Truncate(64 * 1024 * 1024) // 64 MiB
	flashOneFile.Close()

	return flashZeroPath, flashOnePath
}

func getNemuPath(t *testing.T) string {
	u, err := user.Current()
	if err != nil {
		t.Errorf("Error getting current user: %v", err)
		os.Exit(1)
	}

	return path.Join(u.HomeDir, "build-aarch64", "aarch64-softmmu", "qemu-system-aarch64")
}

func (q *qemuTest) launchQemu(ctx context.Context, monitorSocketCh chan string, t *testing.T) {
	virtConsoleLogFile, err := ioutil.TempFile("", "nemu-test")
	if err != nil {
		t.Fatalf("Error creating temporary for console output: %v", err)
	}
	defer os.Remove(virtConsoleLogFile.Name())

	primaryDiskImagePath := q.createPrimaryDiskImage(t)
	defer os.Remove(primaryDiskImagePath)
	cloudInitImagePath := q.createCloudInitImage(t)
	defer os.Remove(cloudInitImagePath)

	flashZeroPath, flashOnePath := createFlashImages(t)
	defer os.Remove(flashZeroPath)
	defer os.Remove(flashOnePath)

	q.sshPort = allocateSSHPort()

	q.params = []string{
		"-machine", fmt.Sprintf("%s,accel=kvm,gic-version=host", q.machine),
		"-m", "512",
		"-cpu", "host",
		"-nographic",
		"-no-user-config",
		"-nodefaults",
		"-pflash", flashZeroPath,
		"-pflash", flashOnePath,
		"-drive", fmt.Sprintf("file=%s,if=none,id=drive-virtio-disk0,format=qcow2", primaryDiskImagePath),
		"-device", "virtio-blk-device,scsi=off,drive=drive-virtio-disk0,id=virtio-disk0",
		"-device", "virtio-blk-device,drive=cloud",
		"-drive", fmt.Sprintf("if=none,id=cloud,file=%s,format=raw", cloudInitImagePath),
		"-netdev", fmt.Sprintf("user,id=mynet0,hostfwd=tcp::%d-:22,hostname=nemuvm", q.sshPort),
		"-device", "virtio-net-device,netdev=mynet0",
		"-device", "virtio-serial-device,id=virtio-serial0",
		"-device", "virtconsole,chardev=charconsole0,id=console0",
		"-chardev", fmt.Sprintf("file,id=charconsole0,path=%s,server,nowait", virtConsoleLogFile.Name()),
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

		t.Logf("\n\n==== virt console output: ===\n\n")
		data, err := ioutil.ReadAll(virtConsoleLogFile)
		if err != nil {
			t.Errorf("Error reading virt console output: %v", err)
		}
		t.Log(string(data))
	}
}

const xenialArmDiskImage = "xenial-server-cloudimg-arm64-uefi1.img"

var ubuntuArmOnly = []distro{
	{
		name:      "xenial",
		image:     xenialArmDiskImage,
		cloudInit: cloudInitUbuntu,
	},
}

var armMachine = []string{"virt"}
var tests = []testConfig{
	{
		name:     "Shutdown",
		testFunc: testShutdown,
		distros:  ubuntuArmOnly,
		machines: armMachine,
	},
	{
		name:     "Reboot",
		testFunc: testReboot,
		distros:  ubuntuArmOnly,
		machines: armMachine,
	},
	{
		name:     "QMPQuit",
		testFunc: testQMPQuit,
		distros:  ubuntuArmOnly,
		machines: armMachine,
	},
}
