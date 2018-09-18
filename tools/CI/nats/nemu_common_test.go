package main

import (
	"context"
	"testing"
	"time"
)

func testShutdown(ctx context.Context, q *qemuTest, t *testing.T) {
	time.Sleep(time.Second * 15)
	q.runCommandBySSH("sudo shutdown -h now", t)
}

func testReboot(ctx context.Context, q *qemuTest, t *testing.T) {
	time.Sleep(time.Second * 15)
	q.runCommandBySSH("sudo reboot", t)
	time.Sleep(time.Second * 15)
	q.runCommandBySSH("sudo shutdown -h now", t)
}

func testQMPQuit(ctx context.Context, q *qemuTest, t *testing.T) {
	time.Sleep(time.Second * 15)
	err := q.qmp.ExecuteQuit(ctx)
	if err != nil {
		t.Errorf("Error quiting via QMP: %v", err)
	}
}
