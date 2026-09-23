target("qemu-" .. inferno_target)
	add_files("gdbstub.c", "cpu64.c", "gdbstub64.c", "arm-qmp-cmds.c")
	if has_config("kvm") then
		add_files("hyp_gdbstub.c", "kvm.c")
	end
	if has_config("hvf") then
		add_files("hyp_gdbstub.c", "hvf/hvf.c")
	end
	if has_config("kvm") or has_config("hvf") then
		add_files("emulate/aarch64.c")
	end
	add_files(
		"../../gdb-xml/aarch64-core.xml",
		"../../gdb-xml/aarch64-fpu.xml",
		"../../gdb-xml/arm-core.xml",
		"../../gdb-xml/arm-vfp.xml",
		"../../gdb-xml/arm-vfp3.xml",
		"../../gdb-xml/arm-vfp-sysregs.xml",
		"../../gdb-xml/arm-neon.xml",
		"../../gdb-xml/aarch64-pauth.xml")
target_end()

target("common_arm")
	add_files("mmuidx.c")
target_end()

target("system_arm")
	add_files("cpu.c", "arm-powerctl.c", "cortex-regs.c", "cpregs-pmu.c", "cpu-irq.c", "debug_helper.c", "helper.c",
		"ptw.c", "vfp_fpscr.c")
	if not has_config("kvm") then
		add_files("kvm-stub.c")
	end
	if not has_config("hvf") then
		add_files("hvf-stub.c")
	end
	if not has_config("tcg") then
		add_files("tcg-stubs.c")
	end
target_end()

if has_config("tcg") then
	includes("tcg")
end
