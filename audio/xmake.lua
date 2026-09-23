target("system")
	add_files("audio.c", "audio-hmp-cmds.c", "mixeng.c", "noaudio.c", "wavaudio.c", "wavcapture.c")
	if has_config("coreaudio") then
		add_files("coreaudio.m")
	end
	if has_config("dsound") then
		add_files("dsoundaudio.c", "audio_win_int.c")
	end
	if has_package("alsa") then
		add_files("alsaaudio.c")
	end
	if has_config("oss") then
		add_files("ossaudio.c")
	end
	if has_package("pa") then
		add_files("paaudio.c")
	end
	if has_package("sdl") then
		add_files("sdlaudio.c")
	end
	if has_package("jack") then
		add_files("jackaudio.c")
	end
	if has_package("sndio") then
		add_files("sndioaudio.c")
	end
	if has_package("pipewire") then
		add_files("pwaudio.c")
	end
target_end()
