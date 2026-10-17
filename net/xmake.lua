target("system")
	add_files("announce.c", "checksum.c", "dgram.c", "eth.c", "hub.c", "net-hmp-cmds.c", "net.c", "queue.c", "socket.c",
		"stream.c", "stream_data.c", "util.c")
	if is_plat("windows", "mingw") then
		add_files("tap-win32.c")
	elseif is_plat("linux") then
		add_files("tap.c", "tap-linux.c")
	elseif is_plat("macosx", "bsd") then
		add_files("tap.c", "tap-bsd.c")
	else
		add_files("tap.c", "tap-stub.c")
	end
	if has_config("__CONFIG_L2TPV3") then
		add_files("l2tpv3.c")
	end
	if has_package("slirp") then
		add_files("slirp.c")
	end
	if has_config("vde") then
		add_files("vde.c")
	end
	if has_config("__CONFIG_NETMAP") then
		add_files("netmap.c")
	end
	if has_config("vmnet") then
		add_files("vmnet-common.m", "vmnet-bridged.m", "vmnet-host.c", "vmnet-shared.c")
	end
target_end()
