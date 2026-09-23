target("qemuutil")
	add_files("cpu-get-clock.c", "error-printf.c", "fdset.c", "iothread-lock.c", "is-daemonized.c", "monitor-core.c",
		"trace-control.c")
	add_files("bdrv-next-monitor-owned.c", "blockdev-close-all-bdrv-states.c", "change-state-handler.c",
		"iothread-lock-block.c")
	add_files("cpus-virtual-clock.c", "graph-lock.c", "qemu-timer-notify-cb.c")
	if inferno_have_io_uring then
		add_files("io_uring.c")
	end
	if has_config("linux_aio") then
		add_files("linux-aio.c")
	end
	add_files("monitor-internal.c", "qmp-command-available.c", "vm-stop.c")
	add_files("target-get-monitor-def.c", "target-monitor-defs.c", "win32-kbd-hook.c")
target_end()
