target("genh")
	set_values("inferno.qapi_modules",
		"authz", "block", "block-core", "block-export", "char", "common", "compat", "control", "crypto", "error",
		"introspect", "job", "machine-common", "machine", "misc", "net", "pragma", "qom", "run-state", "sockets",
		"stats", "trace", "transaction", "yank", "accelerator", "audio", "qdev", "pci", "ui", "usb")
target_end()

target("qemuutil")
	add_files("opts-visitor.c", "qapi-clone-visitor.c", "qapi-dealloc-visitor.c", "qapi-forward-visitor.c",
		"qapi-util.c", "qapi-visit-core.c", "qobject-input-visitor.c", "qobject-output-visitor.c",
		"string-input-visitor.c", "string-output-visitor.c", "qapi-type-helpers.c", "qmp-dispatch.c", "qmp-event.c",
		"qmp-registry.c")
target_end()
