target("block")
	add_rules("inferno.block_gen")
	set_values("inferno.block_gen_headers",
		"include/block/block-io.h",
		"include/block/dirty-bitmap.h",
		"include/block/block_int-io.h",
		"include/block/block-global-state.h",
		"include/system/block-backend-global-state.h",
		"include/system/block-backend-io.h",
		"block/coroutines.h")
	add_files("accounting.c", "aio_task.c", "amend.c", "backup.c", "blkdebug.c", "blklogwrites.c", "blkverify.c",
		"block-backend.c", "block-copy.c", "commit.c", "copy-before-write.c", "copy-on-read.c", "create.c", "crypto.c",
		"dirty-bitmap.c", "filter-compress.c", "graph-lock.c", "io.c", "mirror.c", "null.c", "preallocate.c",
		"progress_meter.c", "qapi.c", "qcow2.c", "qcow2-bitmap.c", "qcow2-cache.c", "qcow2-cluster.c",
		"qcow2-refcount.c", "qcow2-snapshot.c", "qcow2-threads.c", "quorum.c", "raw-format.c", "reqlist.c",
		"snapshot.c", "snapshot-access.c", "write-threshold.c", "stream.c")
	if is_plat("mingw") then
		add_files("file-win32.c", "win32-aio.c")
	else
		add_files("file-posix.c")
	end
	if has_config("dmg") then
		add_files("dmg.c", "dmg-lzfse.c")
		if has_config("bzip2") then
			add_files("dmg-bz2.c")
		end
	end
	if has_config("linux_aio") then
		add_files("linux-aio.c")
	end
	if inferno_have_io_uring then
		add_files("io_uring.c")
	end
	if has_package("blkio") then
		add_files("blkio.c")
	end
	if has_package("curl") then
		add_files("curl.c")
	end
	if has_package("glusterfs") then
		add_files("gluster.c")
	end
	if has_package("libnfs") then
		add_files("nfs.c")
	end
	if has_package("libssh") then
		add_files("ssh.c")
	end
	if has_config("rbd") then
		add_files("rbd.c")
	end
target_end()

target("system")
	add_files("qapi-system.c")
target_end()

includes("export", "monitor")
