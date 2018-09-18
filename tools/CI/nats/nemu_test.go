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

func allocateSSHPort() uint16 {
	sshPortMutex.Lock()
	res := sshPort
	sshPort++
	sshPortMutex.Unlock()
	return res
}

func (q *qemuTest) getSourceDiskImage(t *testing.T) string {
	u, err := user.Current()
	if err != nil {
		t.Errorf("Error getting current user: %v", err)
		os.Exit(1)
	}

	diskImage := q.diskImage
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

type cloudInitType string

type distro struct {
	name      string
	image     string
	cloudInit cloudInitType
}

type testConfig struct {
	name     string
	testFunc func(ctx context.Context, q *qemuTest, t *testing.T)
	distros  []distro
	machines []string
}

const (
	cloudInitClear  cloudInitType = "clear"
	cloudInitUbuntu cloudInitType = "ubuntu"
)

func TestNemu(t *testing.T) {
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			t.Parallel()
			for _, m := range test.machines {
				t.Run(m, func(t *testing.T) {
					t.Parallel()
					for _, d := range test.distros {
						t.Run(d.name, func(t *testing.T) {
							t.Parallel()
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

							test.testFunc(ctx, &q, t)

							<-q.doneCh
							cancelFunc()
						})
					}
				})
			}
		})
	}
}
