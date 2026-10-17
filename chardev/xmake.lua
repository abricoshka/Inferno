target("chardev")
	add_files("char-fe.c", "char-file.c", "char-io.c", "char-mux.c", "char-hub.c", "char-null.c", "char-pipe.c",
		"char-ringbuf.c", "char-serial.c", "char-socket.c", "char-stdio.c", "char-udp.c", "char.c")
	if is_plat("windows", "mingw") then
		add_files("char-console.c", "char-win-stdio.c", "char-win.c")
	else
		add_files("char-fd.c", "char-parallel.c", "char-pty.c")
	end
target_end()

target("system")
	add_files("char-hmp-cmds.c")
target_end()
