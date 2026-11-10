//go:build !windows

package main

import "os/exec"

const exeSuffix = ""

func hideWindow(c *exec.Cmd) {}
