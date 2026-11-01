target("system")
	add_files("clipboard.c", "console.c", "cursor.c", "input-keymap.c", "input-legacy.c", "input-barrier.c", "input.c",
		"kbd-state.c", "keymaps.c", "ui-hmp-cmds.c", "ui-qmp-cmds.c", "util.c", "console-vc.c")
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
	if has_config("wxwidgets") then
		add_files("wxwidgets.c", "wxwidgets.cpp")
        add_packages("wxwidgets")
	end
target_end()
