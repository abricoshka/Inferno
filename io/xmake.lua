target("io")
	add_files("channel-buffer.c", "channel-command.c", "channel-file.c", "channel-null.c", "channel-socket.c",
		"channel-tls.c", "channel-util.c", "channel-watch.c", "channel-websock.c", "channel.c", "dns-resolver.c",
		"net-listener.c", "task.c")
target_end()
