// 3DS-web: Azahar の libretro コアをブラウザで動かすための最小フロントエンド。
// Licensed under GPLv2 or any later version (Azahar と同じ)。
//
// いまの段階の目的は「リンクが通り、ROM を読み込んで retro_run が回る」こと。
// 音声と入力は未接続（TODO）。

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <libretro.h>

// ソフトウェア描画: XRGB8888 を RGBA に並べ替えて 2D キャンバスへ転送する。
// clang-format off
EM_JS(void, BlitSoftwareFrame, (const void* data, unsigned width, unsigned height, unsigned pitch), {
    const src = data >>> 2;
    const stride = pitch >>> 2;
    let st = Module.__sw;
    if (!st || st.width !== width || st.height !== height) {
        const canvas = Module.canvas;
        canvas.width = width;
        canvas.height = height;
        const ctx = canvas.getContext('2d');
        const image = ctx.createImageData(width, height);
        st = Module.__sw = {width, height, ctx, image, out: new Uint32Array(image.data.buffer)};
    }
    const heap = HEAPU32;
    const out = st.out;
    for (let y = 0, o = 0; y < height; y++) {
        for (let x = 0, i = src + y * stride; x < width; x++, i++, o++) {
            const px = heap[i];
            out[o] = 0xFF000000 | (px & 0xFF00) | ((px >>> 16) & 0xFF) | ((px & 0xFF) << 16);
        }
    }
    st.ctx.putImageData(st.image, 0, 0);
});
// clang-format on

namespace {

retro_hw_render_callback hw_render{};
bool hw_render_set = false;
bool game_loaded = false;
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl_context = 0;

const char* const SYSTEM_DIR = "/azahar/system";
const char* const SAVE_DIR = "/azahar/save";

void LogPrintf(retro_log_level level, const char* fmt, ...) {
    static const char* const names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[%s] ", names[level <= RETRO_LOG_ERROR ? level : RETRO_LOG_ERROR]);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

uintptr_t GetCurrentFramebuffer() {
    return 0; // 既定のフレームバッファ（canvas）に直接描く
}

retro_proc_address_t GetProcAddress(const char* name) {
    return reinterpret_cast<retro_proc_address_t>(emscripten_webgl_get_proc_address(name));
}

// コア設定の上書き。ここに無いキーはコアの既定値になる。
// JS からは web_set_option() で ROM の読み込み前に変更できる。
std::map<std::string, std::string> options = {
    // WASM では dynarmic JIT が使えない
    {"citra_use_cpu_jit", "disabled"},
    {"citra_use_fastinterp", "enabled"},
    {"citra_graphics_api", "OpenGL"},
    // WebGL2 にはジオメトリシェーダーが無い。頂点シェーダーへの組み込みが済むまで、
    // 頂点処理は CPU（PICA シェーダーのインタプリタ）で行う
    {"citra_use_hw_shader", "disabled"},
};

const char* GetVariable(const char* key) {
    const auto it = options.find(key);
    return it != options.end() ? it->second.c_str() : nullptr;
}

bool Environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = LogPrintf;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char**>(data) = SYSTEM_DIR;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = SAVE_DIR;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        *static_cast<retro_hw_context_type*>(data) = RETRO_HW_CONTEXT_OPENGLES3;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        auto* cb = static_cast<retro_hw_render_callback*>(data);
        cb->get_current_framebuffer = GetCurrentFramebuffer;
        cb->get_proc_address = GetProcAddress;
        hw_render = *cb;
        hw_render_set = true;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* var = static_cast<retro_variable*>(data);
        var->value = GetVariable(var->key);
        return var->value != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
        *static_cast<bool*>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        std::fprintf(stderr, "[MSG] %s\n", static_cast<const retro_message*>(data)->msg);
        return true;
    default:
        return false;
    }
}

void VideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
    // HW レンダリングでは canvas に描画済みなので何もしない
    if (data == nullptr || data == RETRO_HW_FRAME_BUFFER_VALID) {
        return;
    }
    BlitSoftwareFrame(data, width, height, static_cast<unsigned>(pitch));
}

void AudioSample(int16_t, int16_t) {}

size_t AudioSampleBatch(const int16_t*, size_t frames) {
    return frames; // TODO: Web Audio へ渡す
}

void InputPoll() {}

int16_t InputState(unsigned, unsigned, unsigned, unsigned) {
    return 0; // TODO: キーボード・ゲームパッド・タッチ
}

bool CreateGLContext() {
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion = 2;
    attrs.minorVersion = 0;
    attrs.alpha = false;
    attrs.depth = true;
    attrs.stencil = true;
    attrs.antialias = false;
    attrs.preserveDrawingBuffer = false;
    attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;

    gl_context = emscripten_webgl_create_context("#canvas", &attrs);
    if (gl_context <= 0) {
        std::fprintf(stderr, "WebGL2 コンテキストを作成できません (%d)\n", static_cast<int>(gl_context));
        return false;
    }
    return emscripten_webgl_make_context_current(gl_context) == EMSCRIPTEN_RESULT_SUCCESS;
}

// 速度計測: 直近 60 フレームの retro_run の平均処理時間。
// 実機は約 59.83fps なので、1 フレーム 16.7ms 以内なら等速。
double frame_ms_sum = 0.0;
unsigned frame_count = 0;
double last_avg_frame_ms = 0.0;
unsigned total_frames = 0;

void MainLoop() {
    if (!game_loaded) {
        return;
    }
    const double start = emscripten_get_now();
    retro_run();
    frame_ms_sum += emscripten_get_now() - start;
    ++total_frames;
    if (++frame_count == 60) {
        last_avg_frame_ms = frame_ms_sum / frame_count;
        frame_ms_sum = 0.0;
        frame_count = 0;
    }
}

} // namespace

extern "C" {

// JS から呼ぶ: コア設定を上書きする（ROM の読み込み前に呼ぶ）。
EMSCRIPTEN_KEEPALIVE void web_set_option(const char* key, const char* value) {
    options[key] = value;
}

// JS から呼ぶ: 直近 60 フレームの平均処理時間（ms）。まだ計測できていなければ 0。
EMSCRIPTEN_KEEPALIVE double web_get_frame_ms() {
    return last_avg_frame_ms;
}

// JS から呼ぶ: 起動してから実行したフレーム数。
EMSCRIPTEN_KEEPALIVE unsigned web_get_frame_count() {
    return total_frames;
}

// JS から呼ぶ: 仮想ファイルシステム上の ROM を読み込んで実行を始める。
EMSCRIPTEN_KEEPALIVE bool web_load_game(const char* path) {
    if (game_loaded) {
        retro_unload_game();
        game_loaded = false;
    }

    retro_game_info info{};
    info.path = path;
    if (!retro_load_game(&info)) {
        std::fprintf(stderr, "retro_load_game に失敗: %s\n", path);
        return false;
    }

    if (hw_render_set) {
        if (gl_context == 0 && !CreateGLContext()) {
            return false;
        }
        if (hw_render.context_reset) {
            hw_render.context_reset();
        }
    }

    retro_system_av_info av{};
    retro_get_system_av_info(&av);
    emscripten_set_canvas_element_size("#canvas", av.geometry.base_width, av.geometry.base_height);
    std::printf("読み込み完了: %ux%u %.2ffps\n", av.geometry.base_width, av.geometry.base_height,
                av.timing.fps);

    game_loaded = true;
    return true;
}

} // extern "C"

int main() {
    retro_set_environment(Environment);
    retro_set_video_refresh(VideoRefresh);
    retro_set_audio_sample(AudioSample);
    retro_set_audio_sample_batch(AudioSampleBatch);
    retro_set_input_poll(InputPoll);
    retro_set_input_state(InputState);
    retro_init();

    retro_system_info sys{};
    retro_get_system_info(&sys);
    std::printf("%s %s 起動\n", sys.library_name, sys.library_version);

    emscripten_set_main_loop(MainLoop, 0, false);
    return 0;
}
