target("system")
	add_files("clipboard.c", "console.c", "cursor.c", "input-keymap.c", "input-legacy.c", "input-barrier.c", "input.c",
		"kbd-state.c", "keymaps.c", "qemu-pixman.c", "ui-hmp-cmds.c", "ui-qmp-cmds.c", "util.c", "console-vc.c")
	if has_config("cocoa") then
		add_files("cocoa.m")
	end
	if inferno_have_vnc then
		add_files("vnc.c", "vnc-enc-zlib.c", "vnc-enc-hextile.c", "vnc-enc-tight.c", "vnc-palette.c", "vnc-enc-zrle.c",
			"vnc-auth-vencrypt.c", "vnc-ws.c", "vnc-jobs.c", "vnc-clipboard.c")
		if has_config("vnc_sasl") then
			add_files("vnc-auth-sasl.c")
		end
	else
		add_files("vnc-stubs.c")
	end
	if inferno_have_curses then
		add_files("curses.c")
		add_defines("NCURSES_WIDECHAR=1", {public = true})
	end
	if has_package("gtk") then
		add_files("gtk.c")
		if is_plat("mingw") then
			add_files("win32-kbd-hook.c")
		end
		if has_config("gtk_clipboard") then
			add_files("gtk-clipboard.c")
		end
	end
	if has_package("sdl") then
		add_files("sdl2-2d.c", "sdl2-input.c", "sdl2.c")
	end
	if (has_package("gtk") or has_package("sdl")) and has_package("x11") then
		add_files("x_keymap.c")
	end
target_end()
