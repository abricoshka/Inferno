target("qemuutil")
	add_files("cpu-get-clock.c", "error-printf.c", "fdset.c", "iothread-lock.c", "is-daemonized.c", "monitor-core.c",
		"trace-control.c")
	add_files("bdrv-next-monitor-owned.c", "blk-commit-all.c", "blk-exp-close-all.c", "blockdev-close-all-bdrv-states.c",
		"change-state-handler.c", "get-vm-name.c", "iothread-lock-block.c", "physmem.c", "ram-block.c",
		"runstate-check.c", "uuid.c")
	add_files("cpus-virtual-clock.c", "graph-lock.c", "qemu-timer-notify-cb.c")
	if inferno_have_io_uring then
		add_files("io_uring.c")
	end
	if has_config("linux_aio") then
		add_files("linux-aio.c")
	end
	add_files("monitor-internal.c", "qmp-command-available.c", "qmp-quit.c", "vm-stop.c")
	add_files("target-get-monitor-def.c", "target-monitor-defs.c", "win32-kbd-hook.c", "monitor-cpu.c")
	add_files("gdbstub.c", "hotplug-stubs.c", "sysbus.c")
target_end()
