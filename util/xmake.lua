target("qemuutil")
	add_files("osdep.c", "cutils.c", "unicode.c", "qemu-timer-common.c", "thread-context.c")
	if not has_config("__CONFIG_ATOMIC64") then
		add_files("atomic64.c")
	end
	if is_plat("mingw") then
		add_files("aio-win32.c", "event_notifier-win32.c", "oslib-win32.c", "qemu-thread-win32.c")
	else
		add_files("aio-posix.c", "fdmon-poll.c", "compatfd.c", "event_notifier-posix.c", "oslib-posix.c",
			"qemu-thread-posix.c", "memfd.c", "drm.c")
		if has_config("__CONFIG_EPOLL_CREATE1") then
			add_files("fdmon-epoll.c")
		end
		if not is_plat("wasm") then
			add_files("mmap-alloc.c")
		end
	end
	if inferno_have_io_uring then
		add_files("fdmon-io_uring.c")
	end
	add_files("defer-call.c", "envlist.c", "path.c", "module.c", "event.c", "host-utils.c", "bitmap.c", "bitops.c",
		"fifo8.c", "cacheflush.c", "error.c", "error-report.c", "qemu-print.c", "id.c", "qemu-config.c", "notify.c",
		"qemu-option.c", "qemu-progress.c", "keyval.c", "crc32c.c", "crc16.c", "uuid.c", "getauxval.c", "rcu.c",
		"log.c", "qdist.c", "qht.c", "qsp.c", "range.c", "reserved-region.c", "stats64.c", "systemd.c",
		"transactions.c", "guest-random.c", "int128.c", "memalign.c", "interval-tree.c", "lockcnt.c", "crc-ccitt.c")
	if has_config("membarrier") then
		add_files("sys_membarrier.c")
	end
	add_files("aiocb.c", "async.c", "base64.c", "main-loop.c", "qemu-coroutine.c", "qemu-coroutine-lock.c",
		"qemu-coroutine-io.c", "thread-pool.c", "qemu-timer.c", "qemu-sockets.c", "aio-wait.c", "buffer.c",
		"bufferiszero.c", "hbitmap.c", "hexdump.c", "iova-tree.c", "iov.c", "block-helpers.c",
		"qemu-coroutine-sleep.c", "qemu-co-shared-resource.c", "qemu-co-timeout.c", "readline.c", "timed-average.c",
		"yank.c")
	add_files("coroutine-" .. inferno_coroutine_backend .. ".c")
	if has_config("__CONFIG_INOTIFY1") then
		add_files("filemonitor-inotify.c")
	else
		add_files("filemonitor-stub.c")
	end
	if is_plat("linux") then
		add_files("chardev_open.c")
	end
	if inferno_cpu == "aarch64" then
		add_files("cpuinfo-aarch64.c")
	elseif inferno_cpu == "x86" or inferno_cpu == "x86_64" then
		add_files("cpuinfo-i386.c")
	elseif inferno_cpu == "loongarch64" then
		add_files("cpuinfo-loongarch.c")
	elseif inferno_cpu == "ppc" or inferno_cpu == "ppc64" then
		add_files("cpuinfo-ppc.c")
	elseif inferno_cpu == "riscv32" or inferno_cpu == "riscv64" then
		add_files("cpuinfo-riscv.c")
	end
	add_rules("inferno.glib_slice")
target_end()
