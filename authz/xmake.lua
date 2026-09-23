target("authz")
	add_files("base.c", "list.c", "listfile.c", "simple.c")
	if has_config("auth_pam") then
		add_files("pamacct.c")
	end
target_end()
