/*
 * Inferno Filesystem Patcher.
 *
 * Copyright (c) 2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "analyser.hpp"
#include "assembler.hpp"
#include "patcher.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <CLI/CLI.hpp>

namespace
{

    class CoreImagePatches
    {
        CacheAnalyser::Image image;
        Assembler&           assembler;

    public:
        CoreImagePatches(const CacheAnalyser& analyser, Assembler& assembler) :
            image(analyser.find_image(FrameworkMatch("CoreImage"))),
            assembler(assembler)
        { }

        template<typename... T>
        void patch_ret_false(T... alts)
        {
            auto addr = this->image.resolve_sym(alts...);
            this->assembler.write_movz_incr(this->image.file, this->image.header, addr, GPReg::R0, false, 0);
            this->assembler.write_ret(this->image.file, this->image.header, addr);
        }

        void patch_ret_true(const char* sym)
        {
            auto addr = this->image.resolve_sym(sym);
            this->assembler.write_movz_incr(this->image.file, this->image.header, addr, GPReg::R0, false, 1);
            this->assembler.write_ret(this->image.file, this->image.header, addr);
        }

        void apply(const bool opengles)
        {
            // Stop usage of Metal.
            this->patch_ret_false("_CI_ENABLE_METAL_GPU");
            this->patch_ret_false("_CI_ENABLE_METAL_BLIT");
            this->patch_ret_false("_CI_ENABLE_MPS");
            this->patch_ret_false("_CI_ENABLE_METAL_CONVERT");
            this->patch_ret_false("_CI_ENABLE_METAL_CAPTURE");
            this->patch_ret_false("_CI_ENABLE_METAL_LABEL");
            this->patch_ret_false("_CI_ENABLE_METAL_IMAGEBLOCKS");
            try {
                this->patch_ret_false("_CI_HARVEST_SPECIALIZED_MTL_FUNCTIONS");
            }
            catch (const std::out_of_range& e) {
                std::cerr << "Warning: " << e.what() << ".\n";
            }
            try {
                this->patch_ret_false("_CI_USE_MTL_DAG_FOR_CIKL_SRC");
            }
            catch (const std::out_of_range& e) {
                std::cerr << "Warning: " << e.what() << ".\n";
            }

            if (!opengles) {
                // Stop crash from assumption of OpenGLES availability.
                this->patch_ret_false("_wrapGLIsUsable",
                                      "+[CIContext(Internal) internalContextWithEAGLContext:options:]");
                try {
                    this->patch_ret_false("_wrapGLIsAccelerated");
                }
                catch (const std::out_of_range& e) {
                    std::cerr << "Warning: " << e.what() << ".\n";
                }

                // Force CoreImage software rendering on iOS 18/26.
                try {
                    this->patch_ret_true("_isWidget");
                }
                catch (const std::out_of_range& e) {
                    std::cerr << "Warning: " << e.what() << ".\n";
                }
            }
        }
    };

    class QuartzCorePatches
    {
        CacheAnalyser::Image image;
        Assembler&           assembler;

        auto function_size(const std::uint64_t start) const -> std::uint64_t
        {
            std::uint64_t end = -1ULL;
            for (const auto& sym : this->image.symbols) {
                if (sym.second > start && sym.second < end) { end = sym.second; }
            }
            return end - start;
        }

        void fix_async_dispatcher(std::istream& stream, const std::uint64_t renderer, const char* sym)
        {
            const auto    start = this->image.resolve_sym(sym);
            std::uint64_t renderer_call;
            try {
                renderer_call = Assembler::find_bl_incr(
                    stream, this->image.header, start, renderer, false,
                    std::uint32_t(std::min(this->function_size(start) / 4, std::uint64_t(0x400))));
            }
            catch (std::out_of_range& e) {
                std::cout << "`" << sym << "` doesn't call `CA::OGL::AsynchronousDispatcher::renderer`, skipping.\n";
                return;
            }

            try {
                Assembler::find_cbz(stream, this->image.header, renderer_call, true, false, 1);
                std::cout << "Detected fixed `CA::OGL::AsynchronousDispatcher` logic, skipping `" << sym << "`.\n";
            }
            catch (std::out_of_range& e) {
                this->assembler.write_nop_incr(this->image.file, this->image.header, renderer_call);
                this->assembler.write_nop_incr(this->image.file, this->image.header, renderer_call);
                this->assembler.write_nop_incr(this->image.file, this->image.header, renderer_call);
                this->assembler.write_nop(
                    this->image.file, this->image.header,
                    Assembler::find_blra(stream, this->image.header, renderer_call, true, false, false, 4));
            }
        }

        void track_iosurface_surfaces(std::istream& stream)
        {
            static constexpr std::uint64_t                            FUNC_SIZE = 0xC0;
            static constexpr std::uint32_t                            BL = 0;    // Placeholder for the next callee.
            static constexpr std::array<std::uint32_t, FUNC_SIZE / 4> CODE{
                0xD503237F,    // pacibsp
                0xA9BB57F6,    // stp x22, x21, [sp, #-0x50]!
                0xA9014FF4,    // stp x20, x19, [sp, #0x10]
                0xA9027BFD,    // stp fp, lr, [sp, #0x20]
                0x910083FD,    // add fp, sp, #0x20
                0x7100043F,    // cmp w1, #1
                0x54000121,    // b.ne fail
                0xA9030FE2,    // stp x2, x3, [sp, #0x30]
                0xAA0403F3,    // mov x19, x4
                0xAA0003F4,    // mov x20, x0
                0xAA0403E0,    // mov x0, x4
                0x52800001,    // mov w1, #0
                0xD2800002,    // mov x2, #0
                BL,            // bl IOSurfaceLock
                0x340000E0,    // cbz w0, ok
                0xD2800015,    // fail: mov x21, #0
                0xAA1503E0,    // ret: mov x0, x21
                0xA9427BFD,    // ldp fp, lr, [sp, #0x20]
                0xA9414FF4,    // ldp x20, x19, [sp, #0x10]
                0xA8C557F6,    // ldp x22, x21, [sp], #0x50
                0xD65F0FFF,    // retab
                0xAA1303E0,    // ok: mov x0, x19
                BL,            // bl IOSurfaceGetBaseAddress
                0xAA0003F5,    // mov x21, x0
                0xAA1303E0,    // mov x0, x19
                BL,            // bl IOSurfaceGetBytesPerRow
                0xAA0003F6,    // mov x22, x0
                0xAA1303E0,    // mov x0, x19
                BL,            // bl IOSurfaceGetWidth
                0xF90023E0,    // str x0, [sp, #0x40]
                0xAA1303E0,    // mov x0, x19
                BL,            // bl IOSurfaceGetHeight
                0xAA0003E6,    // mov x6, x0
                0xAA1503E1,    // mov x1, x21
                0xAA1603E2,    // mov x2, x22
                0xA94313E3,    // ldp x3, x4, [sp, #0x30]
                0xF94023E5,    // ldr x5, [sp, #0x40]
                0x52800007,    // mov w7, #0
                BL,            // bl SWContext::create_surface_from_data
                0xAA0003F5,    // mov x21, x0
                0xB4FFFD00,    // cbz x0, ret
                0xF90062B3,    // str x19, [x21, #0xc0]
                0xAA1303E0,    // mov x0, x19
                BL,            // bl CFRetain
                0xAA1403E0,    // mov x0, x20
                0xAA1503E1,    // mov x1, x21
                BL,            // bl Context::track_surface
                0x17FFFFE1,    // b ret
            };

            // iOS <=18 doesn't track surfaces.
            const auto track_surface = this->image.symbols.find("__ZN2CA3OGL7Context13track_surfaceEPNS0_7SurfaceE");
            if (track_surface == this->image.symbols.end()) { return; }

            const auto start =
                this->image.resolve_sym("__ZN2CA3OGL9SWContext29create_surface_from_iosurfaceEiiiP11__IOSurface");

            std::array<std::uint64_t, 8> callees{};
            auto                         bl = start;
            for (std::size_t i = 0; i < callees.size() - 1; ++i) {
                bl          = Assembler::find_bl(stream, this->image.header, bl, -1ULL, false,
                                                 std::uint32_t((start + FUNC_SIZE - bl) / 4));
                callees[i]  = Assembler::read_bl_target(stream, this->image.header, bl);
                bl         += 4;
            }
            if (bl != start + FUNC_SIZE - 4
                || callees[5] != this->image.resolve_sym("__ZN2CA3OGL9SWContext24create_surface_from_dataEPvliiiib"))
            {
                throw std::runtime_error("unexpected `SWContext::create_surface_from_iosurface` layout");
            }
            callees.back() = track_surface->second;

            auto address = start;
            auto callee  = callees.begin();
            for (const auto inst : CODE) {
                if (inst == BL) {
                    this->assembler.write_bl_incr(this->image.file, this->image.header, address, *callee++);
                }
                else {
                    this->assembler.write_inst_incr(this->image.file, this->image.header, address, inst);
                }
            }
        }

        void nullify(std::uint64_t addr)
        {
            this->assembler.write_movz_incr(this->image.file, this->image.header, addr, GPReg::R0, true, 0);
            this->assembler.write_ret(this->image.file, this->image.header, addr);
        }

        template<typename... T>
        void nullify(T... alts)
        { this->nullify(this->image.resolve_sym(alts...)); }

    public:
        QuartzCorePatches(const CacheAnalyser& analyser, Assembler& assembler) :
            image(analyser.find_image(FrameworkMatch("QuartzCore"))),
            assembler(assembler)
        { }

        void apply(const bool opengles)
        {
            // Neutralise CIF10 support which also neutralises framebuffer compression.
            this->assembler.write_ret(
                this->image.file, this->image.header,
                this->image.resolve_sym("___CADeviceSupportsCIF10_block_invoke", "___CADeviceUseCIF10_block_invoke"));

            if (!opengles) {
                // iOS <=14, bug in two functions: a missing null check on return value of `::renderer` causing a crash.
                const auto asyncDispatchRenderer =
                    this->image.resolve_sym("__ZN2CA3OGL22AsynchronousDispatcher8rendererEv");
                auto cache_file = this->image.file.open();
                fix_async_dispatcher(cache_file, asyncDispatchRenderer,
                                     "__ZN2CA3OGL22AsynchronousDispatcher10stop_timerEv");
                fix_async_dispatcher(cache_file, asyncDispatchRenderer,
                                     "__ZN2CA3OGLL17release_iosurfaceEP11__IOSurface");

                // Always return null on these functions that will return null regardless, to reduce load.
                this->nullify("__ZN2CA12WindowServer10EAGLServer8rendererEv",
                              "__ZN2CA12WindowServer11AccelServer8rendererEv");
                this->nullify(asyncDispatchRenderer);

                // Unlike `MetalContext`, `SWContext` doesn't track IOSurface surfaces, which `push_surface` asserts on.
                try {
                    this->track_iosurface_surfaces(cache_file);
                }
                catch (const std::exception& e) {
                    std::cerr << "Warning: Failed to fix `SWContext` IOSurface surface tracking: " << e.what() << ".\n";
                }
            }
        }
    };

    class SpringBoardFoundationPatches
    {
        CacheAnalyser::Image image;
        Assembler&           assembler;

    public:
        SpringBoardFoundationPatches(const CacheAnalyser& analyser, Assembler& assembler) :
            image(analyser.find_image(PrivateFrameworkMatch("SpringBoardFoundation"))),
            assembler(assembler)
        { }

        void apply()
        {
            // Force return true, fixes wallpaper settings crash due to missing Metal rendering support.
            auto should_use_xpc_service_for_rendering =
                this->image.resolve_sym("+[SBFCARenderer shouldUseXPCServiceForRendering]");
            this->assembler.write_movz_incr(this->image.file, this->image.header, should_use_xpc_service_for_rendering,
                                            GPReg::R0, false, 1);
            this->assembler.write_ret(this->image.file, this->image.header, should_use_xpc_service_for_rendering);
        }
    };

    class CMCapturePatches
    {
        CacheAnalyser::Image image;
        Assembler&           assembler;

        static auto find_image(const CacheAnalyser& analyser) -> CacheAnalyser::Image
        {
            try {
                return analyser.find_image(PrivateFrameworkMatch("CMCapture"));
            }
            catch (const std::out_of_range&) {
                return analyser.find_image(PrivateFrameworkMatch("Celestial"));
            }
        }

    public:
        CMCapturePatches(const CacheAnalyser& analyser, Assembler& assembler) :
            image(find_image(analyser)),
            assembler(assembler)
        { }

        void apply()
        {
            // Neutralise shader precompilation, which requires Metal support.
            try {
                this->assembler.write_ret(this->image.file, this->image.header,
                                          this->image.resolve_sym("_FigPreloadShaders", "_FigCapturePreloadShaders"));
                this->assembler.write_ret(this->image.file, this->image.header,
                                          this->image.resolve_sym("_FigWaitForPreloadShadersCompletion",
                                                                  "_FigCaptureWaitForPreloadShadersCompletion"));
            }
            catch (const std::exception& e) {
                std::cerr << "Warning: " << e.what() << ".\n";
            }
        }
    };

    class NeutrinoCorePatches
    {
        CacheAnalyser::Image objc_image;
        CacheAnalyser::Image image;
        Assembler&           assembler;

    public:
        NeutrinoCorePatches(const CacheAnalyser& analyser, Assembler& assembler) :
            objc_image(analyser.find_image(ImageMatch("/usr/lib/libobjc.A.dylib"))),
            image(analyser.find_image(PrivateFrameworkMatch("NeutrinoCore"), true)),
            assembler(assembler)
        { }

        void apply()
        {
            // There's either Metal or software rendering, no OpenGLES.
            const auto objc_alloc_init = this->objc_image.resolve_sym("_objc_alloc_init");
            const auto nu_sw_renderer  = this->image.resolve_objc_class("NUSoftwareRenderer");
            auto       address = this->image.resolve_sym("-[NUDevice_iOS _newRendererWithCIContextOptions:error:]",
                                                         "-[NUDevice_iOS _newRendererWithOptions:error:]");
            this->assembler.write_adrp_add_incr(this->image.file, this->image.header, address, nu_sw_renderer,
                                                GPReg::R0);
            this->assembler.write_adrp_add_incr(this->image.file, this->image.header, address, objc_alloc_init,
                                                GPReg::R1);
            this->assembler.write_blr_incr(this->image.file, this->image.header, address, GPReg::R1);
            this->assembler.write_ret(this->image.file, this->image.header, address);
        }
    };

    class ClockKitUIPatches
    {
        CacheAnalyser::Image image;
        Assembler&           assembler;

    public:
        ClockKitUIPatches(const CacheAnalyser& analyser, Assembler& assembler) :
            image(analyser.find_image(ImageMatch("/System/Library/PrivateFrameworks/ClockKitUI.framework/ClockKitUI"))),
            assembler(assembler)
        { }

        void apply()
        {
            // Stop it from using Metal/NanoUniverse and crashing.
            auto address = this->image.resolve_sym("-[CLKUIMetalQuadView snapshotInRect:scale:time:]");
            this->assembler.write_movz_incr(this->image.file, this->image.header, address, GPReg::R0, true, 0);
            this->assembler.write_ret(this->image.file, this->image.header, address);
        }
    };

    class PosterBoardPatches
    {
        CacheAnalyser::Image image;
        Assembler&           assembler;

    public:
        PosterBoardPatches(const CacheAnalyser& analyser, Assembler& assembler) :
            image(
                analyser.find_image(ImageMatch("/System/Library/PrivateFrameworks/PosterBoard.framework/PosterBoard"))),
            assembler(assembler)
        { }

        void apply()
        {
            // Stop it from using Metal and crashing.
            auto address = this->image.resolve_sym("-[PBFHomeScreenPreviewImageBuilder _imageForView:]");
            this->assembler.write_movz_incr(this->image.file, this->image.header, address, GPReg::R0, true, 0);
            this->assembler.write_ret(this->image.file, this->image.header, address);
        }
    };

    class LibSystemTracePatches
    {
        CacheAnalyser::Image image;
        Assembler&           assembler;

    public:
        LibSystemTracePatches(const CacheAnalyser& analyser, Assembler& assembler) :
            image(analyser.find_image(ImageMatch("/usr/lib/system/libsystem_trace.dylib"))),
            assembler(assembler)
        { }

        void apply()
        {
            auto address = this->image.resolve_sym("__os_trace_is_development_build");
            this->assembler.write_movz_incr(this->image.file, this->image.header, address, GPReg::R0, false, 1);
            this->assembler.write_ret(this->image.file, this->image.header, address);
        }
    };

    // Temporary: without a baseband, these block forever on ARI requests, and their clients with them.
    class LaunchdPatches
    {
        struct Object
        {
            std::uint8_t type;
            std::size_t  count;
            std::size_t  pos;
        };

        Patcher&                  patcher;
        FileRef                   file;
        std::vector<std::uint8_t> data;
        std::size_t               offset_size{0};
        std::size_t               ref_size{0};
        std::size_t               offset_table{0};
        std::uint64_t             top{0};

        auto be(const std::size_t pos, const std::size_t size) const -> std::uint64_t
        {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < size; ++i) { value = (value << 8) | this->data.at(pos + i); }
            return value;
        }

        auto object(const std::uint64_t index) const -> Object
        {
            auto        pos = std::size_t(this->be(this->offset_table + index * this->offset_size, this->offset_size));
            const auto  marker = this->data.at(pos++);
            std::size_t count  = marker & 0xF;
            if (count == 0xF) {
                const auto int_size  = std::size_t(1) << (this->data.at(pos++) & 0xF);
                count                = std::size_t(this->be(pos, int_size));
                pos                 += int_size;
            }
            return {std::uint8_t(marker >> 4), count, pos};
        }

        auto ref(const Object& obj, const std::size_t i) const -> std::uint64_t
        { return this->be(obj.pos + i * this->ref_size, this->ref_size); }

        auto ascii_string(const std::uint64_t index) const -> std::string_view
        {
            const auto obj = this->object(index);
            if (obj.type != 0x5 || obj.pos + obj.count > this->data.size()) { return {}; }
            return {reinterpret_cast<const char*>(this->data.data() + obj.pos), obj.count};
        }

        auto find(const std::uint64_t dict, const std::string_view key) const -> std::optional<std::uint64_t>
        {
            const auto obj = this->object(dict);
            if (obj.type != 0xD) { throw std::runtime_error("not a dictionary"); }
            for (std::size_t i = 0; i < obj.count; ++i) {
                if (this->ascii_string(this->ref(obj, i)) == key) { return this->ref(obj, obj.count + i); }
            }
            return std::nullopt;
        }

        void corrupt(const std::uint64_t index)
        {
            if (this->ascii_string(index).empty()) { throw std::runtime_error("not an ASCII string"); }
            const auto                      obj = this->object(index);
            const std::vector<std::uint8_t> underscores(obj.count, '_');
            this->patcher.write(this->file, std::streamoff(obj.pos), underscores.data(), underscores.size());
        }

    public:
        static constexpr std::array<const char*, 2> PATHS{
            "/System/Library/xpc/launchd.plist",
            "/System/Library/Caches/com.apple.xpc.launchd/mrm.plist",    // iOS <=13
        };

        static auto sig_path(const char* path) -> std::string { return std::string(path) + ".sig"; }

        LaunchdPatches(Storage& storage, Patcher& patcher, const char* path) :
            patcher(patcher),
            file(storage, path)
        {
            auto stream = this->file.open();
            stream.seekg(0, std::ios::end);
            this->data.resize(std::size_t(stream.tellg()));
            read_stream(seek_stream(stream, 0), this->data.data(), std::streamsize(this->data.size()));
            if (this->data.size() < 40
                || std::string_view(reinterpret_cast<const char*>(this->data.data()), 8) != "bplist00")
            {
                throw std::runtime_error("not a binary plist");
            }

            const auto trailer = this->data.size() - 32;
            this->offset_size  = this->data[trailer + 6];
            this->ref_size     = this->data[trailer + 7];
            this->top          = this->be(trailer + 16, 8);
            this->offset_table = std::size_t(this->be(trailer + 24, 8));
        }

        void apply()
        {
            static constexpr std::array<std::string_view, 8> LABELS{
                "com.apple.CommCenter", "com.apple.CommCenterMobileHelper", "com.apple.CommCenterRootHelper",
                "com.apple.locationd",  "com.apple.voicemail.vmd",          "com.apple.bluetoothd",
                "com.apple.nfcd",
            };

            const auto daemons = this->find(this->top, "LaunchDaemons");
            if (!daemons) { throw std::out_of_range("no launch daemons"); }
            const auto jobs = this->object(*daemons);
            for (std::size_t i = 0; i < jobs.count; ++i) {
                const auto job   = this->ref(jobs, jobs.count + i);
                const auto label = this->find(job, "Label");
                if (!label || std::find(LABELS.begin(), LABELS.end(), this->ascii_string(*label)) == LABELS.end()) {
                    continue;
                }

                if (const auto services = this->find(job, "MachServices")) {
                    const auto obj = this->object(*services);
                    for (std::size_t j = 0; j < obj.count; ++j) { this->corrupt(this->ref(obj, j)); }
                }
                if (const auto program = this->find(job, "Program")) { this->corrupt(*program); }
                if (const auto arguments = this->find(job, "ProgramArguments")) {
                    this->corrupt(this->ref(this->object(*arguments), 0));
                }
            }
        }

        static void apply(Storage& storage, Patcher& patcher)
        {
            try {
                LaunchdPatches(storage, patcher, PATHS[0]).apply();
                patcher.sign_detached(FileRef(storage, PATHS[0]), FileRef(storage, sig_path(PATHS[0])));
            }
            catch (const std::exception& e) {
                try {
                    LaunchdPatches(storage, patcher, PATHS[1]).apply();
                    patcher.sign_detached(FileRef(storage, PATHS[1]), FileRef(storage, sig_path(PATHS[1])));
                }
                catch (const std::exception& e_old) {
                    throw std::runtime_error(std::string(e.what()) + "; " + e_old.what());
                }
            }
        }
    };

    class BTAudioHALPluginPatches
    {
    public:
        static constexpr std::array<const char*, 2> PATHS{
            "/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver/BTAudioHALPlugin",
            "/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver/BTAudioHALPlugin",    // iOS <=14
        };

        static auto find(Storage& storage) -> const char*
        {
            for (const auto* path : PATHS) {
                try {
                    static_cast<void>(FileRef(storage, path).open());
                    return path;
                }
                catch (const std::exception&) {
                }
            }
            return nullptr;
        }

        static void apply(Storage& storage, Patcher& patcher)
        {
            static constexpr std::array<std::uint8_t, 8> RET_NULL{
                0x00, 0x00, 0x80, 0xD2,    // mov x0, #0
                0xC0, 0x03, 0x5F, 0xD6,    // ret
            };

            const auto* path = find(storage);
            if (path == nullptr) { throw std::out_of_range("plugin not found"); }

            const FileRef    file(storage, path);
            auto             stream = file.open();
            const MachHeader header(stream);
            if (!header.symtab) { throw std::runtime_error("no symbol table"); }

            const auto& symtab = *header.symtab;
            for (std::uint32_t i = 0; i < symtab.sym_count; ++i) {
                const NList sym(seek_stream(stream, std::streamoff(symtab.sym_off + i * 16)));
                if (sym.strx == 0 || sym.type_flags.bits.type != NList::Type::Section) { continue; }
                if (read_cstr(seek_stream(stream, std::streamoff(symtab.str_off + sym.strx)))
                    != "_AudioServerPlugIn_Create")
                {
                    continue;
                }

                for (const auto& [name, segment] : header.segments) {
                    if (sym.value >= segment.vm_addr && sym.value < segment.vm_addr + segment.file_size) {
                        patcher.write(file, std::streamoff(segment.file_off + (sym.value - segment.vm_addr)), RET_NULL);
                        return;
                    }
                }
            }
            throw std::out_of_range("symbol `_AudioServerPlugIn_Create` not found");
        }
    };

    auto run(const int argc, char* argv[])
    {
        std::filesystem::path disk_path;
        bool                  revert_only   = false;
        bool                  dryRun        = false;
        bool                  unredact_logs = false;
        bool                  opengles      = false;
        bool                  metal         = false;

        CLI::App app{"Inferno Filesystem Patcher"};
        argv = app.ensure_utf8(argv);

        app.add_option("-d,--disk", disk_path, "Patch the disk image IMAGE")->check(CLI::ExistingFile);
        const auto revertOnlyOpt = app.add_flag("-r,--revert", revert_only, "Revert any modifications");
        const auto dryRunOpt     = app.add_flag(
            "-n,--dry-run", dryRun, "Revert any modifications then run patcher, without flushing patches to the disk");
        revertOnlyOpt->excludes(dryRunOpt);
        dryRunOpt->excludes(revertOnlyOpt);
        app.add_flag("--unredact-logs", unredact_logs, "Patch libsystem_trace.dylib to unredact logs");
        const auto openglesOpt =
            app.add_flag("--opengles", opengles, "Apply minimal patches necessary for the lack of Metal rendering");
        const auto metalOpt = app.add_flag("--metal", metal, "Apply no rendering patches");
        openglesOpt->excludes(metalOpt);
        metalOpt->excludes(openglesOpt);

        CLI11_PARSE(app, argc, argv);

        Storage             storage(disk_path);
        DyldStorage         dyld_storage(storage);
        const CacheAnalyser analyser(dyld_storage);

        std::vector<FileRef> targets;
        targets.reserve(analyser.caches.size());
        for (const auto& cache : analyser.caches) { targets.emplace_back(dyld_storage, cache.first); }
        if (analyser.symbols_cache) { targets.emplace_back(dyld_storage, analyser.symbols_cache->first); }
        for (const auto* path : LaunchdPatches::PATHS) {
            targets.emplace_back(storage, path);
            targets.emplace_back(storage, LaunchdPatches::sig_path(path));
        }
        if (const auto* path = BTAudioHALPluginPatches::find(storage)) { targets.emplace_back(storage, path); }

        for (const auto& target : targets) {
            if (Patcher::revert(target)) { std::cout << "Reverted " << target.string() << ".\n"; }
        }

        if (revert_only) {
            std::cout << "Revert completed successfully.\n";
            return 0;
        }

        Patcher   patcher;
        Assembler assembler(patcher);

        std::cout << "Building patches...\n";
        if (!metal) {
            CoreImagePatches(analyser, assembler).apply(opengles);
            QuartzCorePatches(analyser, assembler).apply(opengles);
            SpringBoardFoundationPatches(analyser, assembler).apply();
            CMCapturePatches(analyser, assembler).apply();
            NeutrinoCorePatches(analyser, assembler).apply();
            try {
                ClockKitUIPatches(analyser, assembler).apply();
            }
            catch (const std::exception& e) {
                std::cerr << "Warning: Failed to apply ClockKitUI patches: " << e.what() << ".\n";
            }
            try {
                PosterBoardPatches(analyser, assembler).apply();
            }
            catch (const std::exception& e) {
                std::cerr << "Warning: Failed to apply PosterBoard patches: " << e.what() << ".\n";
            }
        }
        if (unredact_logs) { LibSystemTracePatches(analyser, assembler).apply(); }
        try {
            LaunchdPatches::apply(storage, patcher);
        }
        catch (const std::exception& e) {
            std::cerr << "Warning: Failed to apply launchd patches: " << e.what() << ".\n";
        }
        try {
            BTAudioHALPluginPatches::apply(storage, patcher);
        }
        catch (const std::exception& e) {
            std::cerr << "Warning: Failed to apply BTAudioHALPlugin patches: " << e.what() << ".\n";
        }
        std::cout << "Patches built successfully.\n";

        patcher.rehash();

        patcher.print_changes();

        if (!dryRun) {
            std::cout << "Applying changes...\n";
            patcher.flush();
            std::cout << "Changes applied successfully.\n";
        }

        return 0;
    }

}    // namespace

auto main(const int argc, char* argv[]) -> int
{
    std::cout << std::hex << std::showbase;
    std::cerr << std::hex << std::showbase;

    try {
        return run(argc, argv);
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << ".\n";

        return EXIT_FAILURE;
    }
}
