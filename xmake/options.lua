function inferno_restrict_host(plat, arch)
	return function (option)
		import("core.project.config")
		if option:enabled() and not (is_plat(plat) and (arch == nil or is_arch(arch))) then
			if config.readonly(option:name()) then
				raise("%s is only available on %s%s hosts", option:name(), arch and arch .. " " or "", plat)
			end
			option:enable(false, {force = true})
		end
	end
end

option("tcg")
	set_default(true)
	set_showmenu(true)
	set_category("Accelerators")
	set_description("Enable the TCG accelerator")
	set_configvar("CONFIG_TCG", 1)
option_end()

option("tcg_interpreter")
	set_default(false)
	set_showmenu(true)
	set_category("Accelerators")
	set_description("Use the TCG bytecode interpreter instead of the native backend")
	add_deps("tcg")
	set_configvar("CONFIG_TCG_INTERPRETER", "", {quote = false})
	after_check(function (option)
		if option:enabled() and not option:dep("tcg"):enabled() then
			raise("tcg_interpreter requires tcg")
		end
	end)
option_end()

option("hvf")
	set_default(true)
	set_showmenu(true)
	set_category("Accelerators")
	set_description("Enable the Hypervisor.framework accelerator (arm64 macOS hosts)")
	set_configvar("CONFIG_HVF", 1)
	after_check(inferno_restrict_host("macosx", "arm64"))
option_end()

option("kvm")
	set_default(true)
	set_showmenu(true)
	set_category("Accelerators")
	set_description("Enable the KVM accelerator (arm64 Linux hosts)")
	set_configvar("CONFIG_KVM", 1)
	after_check(inferno_restrict_host("linux", "arm64"))
option_end()

option("coroutine_backend")
	set_default("auto")
	set_showmenu(true)
	set_values("auto", "ucontext", "sigaltstack", "windows", "wasm")
	set_description("Coroutine backend")
option_end()

option("x86_version")
	set_default("1")
	set_showmenu(true)
	set_values("0", "1", "2", "3", "4")
	set_description("Baseline x86 ISA level (x86-64-v<N>); 0 only for 32-bit hosts")
option_end()

option("tools")
	set_default(true)
	set_showmenu(true)
	set_description("Build the host tools (inferno-img, inferno-edid, ...)")
option_end()

option("trace_backends")
	set_default("nop")
	set_showmenu(true)
	set_category("Debugging")
	set_description("Comma-separated list of trace backends",
		"nop, log, simple, syslog, ftrace, dtrace, ust")
option_end()

option("debug_tcg")
	set_default(false)
	set_showmenu(true)
	set_category("Debugging")
	set_description("Enable TCG debugging")
	set_configvar("CONFIG_DEBUG_TCG", "", {quote = false})
option_end()

option("debug_mutex")
	set_default(false)
	set_showmenu(true)
	set_category("Debugging")
	set_description("Enable mutex debugging")
	set_configvar("CONFIG_DEBUG_MUTEX", "", {quote = false})
option_end()

option("debug_graph_lock")
	set_default(false)
	set_showmenu(true)
	set_category("Debugging")
	set_description("Enable graph lock debugging")
	set_configvar("CONFIG_DEBUG_GRAPH_LOCK", "", {quote = false})
option_end()

option("debug_stack_usage")
	set_default(false)
	set_showmenu(true)
	set_category("Debugging")
	set_description("Measure coroutine stack usage (disables the coroutine pool)")
	set_configvar("CONFIG_DEBUG_STACK_USAGE", "", {quote = false})
option_end()

option("qom_cast_debug")
	set_default(true)
	set_showmenu(true)
	set_category("Debugging")
	set_description("Enable run-time QOM cast checks")
	set_configvar("CONFIG_QOM_CAST_DEBUG", "", {quote = false})
option_end()

option("coroutine_pool")
	set_default(true)
	set_showmenu(true)
	set_description("Enable the coroutine freelist")
	add_deps("debug_stack_usage")
	set_configvar("CONFIG_COROUTINE_POOL", "", {quote = false})
	after_check(function (option)
		if option:dep("debug_stack_usage"):enabled() then
			option:enable(false, {force = true})
		end
	end)
option_end()

option("membarrier")
	set_default(false)
	set_showmenu(true)
	set_description("Use the membarrier system call (Linux)")
	set_configvar("CONFIG_MEMBARRIER", "", {quote = false})
	after_check(function (option)
		import("lib.detect.check_csnippets")
		if not option:enabled() then
			return
		end
		if not is_plat("linux") then
			raise("membarrier is only available on Linux hosts")
		end
		if not check_csnippets([[
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>
int main(void) {
    syscall(__NR_membarrier, MEMBARRIER_CMD_QUERY, 0);
    syscall(__NR_membarrier, MEMBARRIER_CMD_SHARED, 0);
    exit(0);
}]]) then
			raise("membarrier system call not available")
		end
	end)
option_end()

option("dmg")
	set_default(true)
	set_showmenu(true)
	set_category("Features")
	set_description("Enable the DMG block driver")
option_end()

option("pkgversion")
	set_default("")
	set_showmenu(true)
	set_category("Directories")
	set_description("Package version string reported by the binaries (default: git describe)")
option_end()

option("prefix")
	set_default("/usr/local")
	set_showmenu(true)
	set_category("Directories")
	set_description("Installation prefix the binaries resolve their data directories against")
option_end()
