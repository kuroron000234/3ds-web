// 3DS-web: Azahar の libretro コアをブラウザで動かすフロントエンド。
// Licensed under GPLv2 or any later version (Azahar と同じ)。
//
// スレッド構成:
// - ブラウザのメインスレッドは DOM・入力・音声だけを扱い、エミュレーションはしない
//   （メインスレッドは Atomics.wait ができず、スレッド待ちが空回りするため）
// - エミュレーションスレッド（pthread）が canvas を OffscreenCanvas として受け取り、
//   WebGL2 コンテキストを持ち、retro_run を 3DS の速度（約 59.83fps）で回す。
//   各フレームの後はワーカーのイベントループに戻る（そこで描画が画面に出て、
//   メインスレッドから委譲された呼び出しも処理される）
// - JS から呼ぶ web_* 関数のうち、コアに触れるものはエミュレーションスレッドへ委譲する
//
// 入力はメインスレッドの JS が共有メモリ（web_input_ptr）へ書き、コアがそれを読む。
// 音声はコアが共有メモリのリングバッファ（web_audio_ptr）へ書き、AudioWorklet が読む。

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <libretro.h>
#include <pthread.h>

// ソフトウェア描画: XRGB8888 を RGBA に並べ替えて 2D キャンバスへ転送する。
// エミュレーションスレッドで呼ばれ、Module.canvas は移譲された OffscreenCanvas。
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

// 次のフレームを delay ms 後に予約する（エミュレーションスレッドのイベントループで実行）。
// 0 以下なら MessageChannel ですぐに戻ってくる（setTimeout(0) の 4ms 下限を避ける）。
EM_JS(void, ScheduleTick, (double delay), {
    let s = Module.__tick;
    if (!s) {
        const ch = new MessageChannel();
        s = Module.__tick = {ch, run: () => _web_tick()};
        ch.port1.onmessage = s.run;
    }
    if (delay <= 0) s.ch.port2.postMessage(0);
    else setTimeout(s.run, delay);
});
// clang-format on

namespace {

retro_hw_render_callback hw_render{};
bool hw_render_set = false;
bool game_loaded = false;
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl_context = 0;
pthread_t emu_thread{};

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

// ---- 音声: 単一生産者（エミュレーションスレッド）・単一消費者（AudioWorklet）のリング ----
// web/index.html の AudioWorklet と配置を合わせる（uint32 単位）:
//   [0] 書き込み位置（フレーム数、単調増加）  [1] 読み出し位置  [2] 容量（フレーム）
//   [3] 溢れて捨てたフレーム数  [4] 標本化周波数  [8..] int16 ステレオのサンプル
constexpr uint32_t AUDIO_FRAMES = 16384; // 32728Hz で約 0.5 秒
struct AudioRing {
    std::atomic<uint32_t> write;
    std::atomic<uint32_t> read;
    uint32_t capacity;
    std::atomic<uint32_t> dropped;
    std::atomic<uint32_t> sample_rate;
    uint32_t pad[3];
    int16_t samples[AUDIO_FRAMES * 2];
};
AudioRing audio_ring{};

size_t AudioSampleBatch(const int16_t* data, size_t frames) {
    const uint32_t w = audio_ring.write.load(std::memory_order_relaxed);
    const uint32_t r = audio_ring.read.load(std::memory_order_acquire);
    const uint32_t space = AUDIO_FRAMES - (w - r);
    const uint32_t n = frames < space ? static_cast<uint32_t>(frames) : space;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t pos = (w + i) % AUDIO_FRAMES;
        audio_ring.samples[pos * 2] = data[i * 2];
        audio_ring.samples[pos * 2 + 1] = data[i * 2 + 1];
    }
    audio_ring.write.store(w + n, std::memory_order_release);
    if (n < frames) {
        audio_ring.dropped.fetch_add(static_cast<uint32_t>(frames - n), std::memory_order_relaxed);
    }
    return frames; // 溢れた分は捨てる（エミュレーションを止めない）
}

void AudioSample(int16_t left, int16_t right) {
    const int16_t frame[2] = {left, right};
    AudioSampleBatch(frame, 1);
}

// ---- 入力: メインスレッドの JS が Atomics.store で書く（int32 単位）----
//   [0] ボタンのビットマスク（1 << RETRO_DEVICE_ID_JOYPAD_*）
//   [1] 左スティック X  [2] 左 Y  [3] 右スティック X  [4] 右 Y（-32767..32767、下が正）
//   [5] ポインター X  [6] ポインター Y（キャンバス全体を -32767..32767）  [7] 押している
std::atomic<int32_t> input_state[8]{};

int32_t In(int i) {
    return input_state[i].load(std::memory_order_relaxed);
}

void InputPoll() {}

int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) {
        return 0;
    }
    switch (device) {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            return static_cast<int16_t>(In(0) & 0xFFFF);
        }
        return id < 16 ? (In(0) >> id) & 1 : 0;
    case RETRO_DEVICE_ANALOG:
        if (index <= RETRO_DEVICE_INDEX_ANALOG_RIGHT && id <= RETRO_DEVICE_ID_ANALOG_Y) {
            return static_cast<int16_t>(In(1 + index * 2 + id));
        }
        return 0;
    case RETRO_DEVICE_POINTER:
        switch (id) {
        case RETRO_DEVICE_ID_POINTER_X:
            return static_cast<int16_t>(In(5));
        case RETRO_DEVICE_ID_POINTER_Y:
            return static_cast<int16_t>(In(6));
        case RETRO_DEVICE_ID_POINTER_PRESSED:
        case RETRO_DEVICE_ID_POINTER_COUNT:
            return In(7) != 0;
        }
        return 0;
    case RETRO_DEVICE_MOUSE:
        return id == RETRO_DEVICE_ID_MOUSE_LEFT ? In(7) != 0 : 0;
    }
    return 0;
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
    // canvas はこのスレッドへ OffscreenCanvas として移譲済み。メインスレッドへの委譲はしない
    attrs.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_DISALLOW;

    gl_context = emscripten_webgl_create_context("#canvas", &attrs);
    if (gl_context <= 0) {
        std::fprintf(stderr, "WebGL2 コンテキストを作成できません (%d)\n", static_cast<int>(gl_context));
        return false;
    }
    return emscripten_webgl_make_context_current(gl_context) == EMSCRIPTEN_RESULT_SUCCESS;
}

// ---- 速度の調整と計測（エミュレーションスレッドだけが触る。結果は atomic で公開）----
// frame_period_ms ごとに 1 フレーム。間はイベントループで寝る（空回りしない）。
double frame_period_ms = 1000.0 / 59.8261;
double next_deadline = 0.0;
bool tick_scheduled = false;

// 直近 60 フレームの retro_run の平均処理時間（16.7ms 以内なら等速を出せる）
double frame_ms_sum = 0.0;
unsigned frame_count = 0;
std::atomic<double> last_avg_frame_ms{0.0};
std::atomic<unsigned> total_frames{0};

// 約 1 秒ごとの窓: 速度（実機比 %）と、エミュレーションスレッドが retro_run にいた割合（%）
double window_start = 0.0;
unsigned window_frames = 0;
double window_busy_ms = 0.0;
std::atomic<double> speed_percent{0.0};
std::atomic<double> busy_percent{0.0};

void Schedule() {
    tick_scheduled = true;
    ScheduleTick(next_deadline - emscripten_get_now());
}

void RunFrame() {
    const double start = emscripten_get_now();
    retro_run();
    const double end = emscripten_get_now();
    const double ms = end - start;
    frame_ms_sum += ms;
    window_busy_ms += ms;
    ++window_frames;
    total_frames.fetch_add(1, std::memory_order_relaxed);
    if (++frame_count == 60) {
        last_avg_frame_ms.store(frame_ms_sum / frame_count, std::memory_order_relaxed);
        frame_ms_sum = 0.0;
        frame_count = 0;
    }
    const double elapsed = end - window_start;
    if (elapsed >= 1000.0) {
        speed_percent.store(window_frames * frame_period_ms / elapsed * 100.0,
                            std::memory_order_relaxed);
        busy_percent.store(window_busy_ms / elapsed * 100.0, std::memory_order_relaxed);
        window_start = end;
        window_frames = 0;
        window_busy_ms = 0.0;
    }
}

// ---- エミュレーションスレッドで実行する処理（メインスレッドから委譲される）----

struct LoadRequest {
    std::string path;
    bool result;
};

// 読み込みの状態（web_load_state）: 0 未開始、1 読み込み中、2 成功、3 失敗
std::atomic<int> load_state{0};

void LoadGameOnEmuThread(void* arg) {
    auto* req = static_cast<LoadRequest*>(arg);
    req->result = false;
    if (game_loaded) {
        retro_unload_game();
        game_loaded = false;
    }

    retro_game_info info{};
    info.path = req->path.c_str();
    if (!retro_load_game(&info)) {
        std::fprintf(stderr, "retro_load_game に失敗: %s\n", req->path.c_str());
        return;
    }

    if (hw_render_set) {
        if (gl_context == 0 && !CreateGLContext()) {
            return;
        }
        if (hw_render.context_reset) {
            hw_render.context_reset();
        }
    }

    retro_system_av_info av{};
    retro_get_system_av_info(&av);
    emscripten_set_canvas_element_size("#canvas", av.geometry.base_width, av.geometry.base_height);
    if (av.timing.fps > 1.0) {
        frame_period_ms = 1000.0 / av.timing.fps;
    }
    audio_ring.sample_rate.store(static_cast<uint32_t>(std::lround(av.timing.sample_rate)));
    std::printf("読み込み完了: %ux%u %.2ffps %.0fHz\n", av.geometry.base_width,
                av.geometry.base_height, av.timing.fps, av.timing.sample_rate);

    game_loaded = true;
    req->result = true;
    window_start = next_deadline = emscripten_get_now();
    window_frames = 0;
    window_busy_ms = 0.0;
    if (!tick_scheduled) {
        Schedule();
    }
}

struct OptionRequest {
    const char* key;
    const char* value;
};

void SetOptionOnEmuThread(void* arg) {
    auto* req = static_cast<OptionRequest*>(arg);
    options[req->key] = req->value;
}

void* EmuThreadMain(void*) {
    retro_set_environment(Environment);
    retro_set_video_refresh(VideoRefresh);
    retro_set_audio_sample(AudioSample);
    retro_set_audio_sample_batch(AudioSampleBatch);
    retro_set_input_poll(InputPoll);
    retro_set_input_state(InputState);
    retro_init();

    retro_system_info sys{};
    retro_get_system_info(&sys);
    std::printf("%s %s 起動（エミュレーションスレッド）\n", sys.library_name, sys.library_version);

    // スレッドを終わらせずにイベントループへ戻る。以後は ScheduleTick と委譲された呼び出しで動く
    emscripten_exit_with_live_runtime();
}

// メインスレッドから: fn をエミュレーションスレッドで実行し、終わるまで待つ。
// 待つ間もメインスレッドは委譲されてきた処理（FS など）を実行するので、行き詰まらない。
void RunOnEmuThread(void (*fn)(void*), void* arg) {
    if (pthread_equal(pthread_self(), emu_thread)) {
        fn(arg);
        return;
    }
    if (!emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emu_thread, fn, arg)) {
        std::fprintf(stderr, "エミュレーションスレッドへの委譲に失敗\n");
    }
}

} // namespace

extern "C" {

// エミュレーションスレッドのイベントループから呼ばれる（ScheduleTick）。
EMSCRIPTEN_KEEPALIVE void web_tick() {
    tick_scheduled = false;
    if (!game_loaded) {
        return;
    }
    double now = emscripten_get_now();
    if (now >= next_deadline) {
        RunFrame();
        next_deadline += frame_period_ms;
        now = emscripten_get_now();
        // 3 フレーム以上遅れたら追いつくのをあきらめる（遅い端末で連続実行し続けないように）
        if (now - next_deadline > frame_period_ms * 3) {
            next_deadline = now;
        }
    }
    Schedule();
}

// JS から呼ぶ: コア設定を上書きする（ROM の読み込み前に呼ぶ）。
EMSCRIPTEN_KEEPALIVE void web_set_option(const char* key, const char* value) {
    OptionRequest req{key, value};
    RunOnEmuThread(SetOptionOnEmuThread, &req);
}

// JS から呼ぶ: 直近 60 フレームの retro_run の平均処理時間（ms）。まだ計測できていなければ 0。
EMSCRIPTEN_KEEPALIVE double web_get_frame_ms() {
    return last_avg_frame_ms.load(std::memory_order_relaxed);
}

// JS から呼ぶ: 起動してから実行したフレーム数。
EMSCRIPTEN_KEEPALIVE unsigned web_get_frame_count() {
    return total_frames.load(std::memory_order_relaxed);
}

// JS から呼ぶ: 直近約 1 秒のエミュレーション速度（実機に対する %、100 で等速）。
EMSCRIPTEN_KEEPALIVE double web_get_speed() {
    return speed_percent.load(std::memory_order_relaxed);
}

// JS から呼ぶ: 直近約 1 秒にエミュレーションスレッドが retro_run にいた時間の割合（%）。
EMSCRIPTEN_KEEPALIVE double web_get_busy() {
    return busy_percent.load(std::memory_order_relaxed);
}

// JS から呼ぶ: 入力の共有領域（int32 × 8、配置は input_state の説明を参照）。
EMSCRIPTEN_KEEPALIVE void* web_input_ptr() {
    return input_state;
}

// JS から呼ぶ: 音声のリングバッファ（配置は AudioRing の説明を参照）。
EMSCRIPTEN_KEEPALIVE void* web_audio_ptr() {
    return &audio_ring;
}

// JS から呼ぶ: 仮想ファイルシステム上の ROM の読み込みをエミュレーションスレッドで始める。
// すぐに戻るので、結果は web_load_state() が 2（成功）か 3（失敗）になるまで待って確かめる。
// メインスレッドを止めると、コアが必要とするメインスレッド側の処理（スレッドの起動、
// ログの出力など）が進まず行き詰まるので、同期では待たない。
EMSCRIPTEN_KEEPALIVE bool web_load_game(const char* path) {
    if (load_state.exchange(1) == 1) {
        return false; // 読み込み中
    }
    auto* req = new LoadRequest{path, false};
    const bool queued = emscripten_proxy_async(
        emscripten_proxy_get_system_queue(), emu_thread,
        [](void* arg) {
            auto* r = static_cast<LoadRequest*>(arg);
            LoadGameOnEmuThread(r);
            load_state.store(r->result ? 2 : 3);
            delete r;
        },
        req);
    if (!queued) {
        delete req;
        load_state.store(3);
    }
    return queued;
}

// JS から呼ぶ: 読み込みの状態。0 未開始、1 読み込み中、2 成功、3 失敗。
EMSCRIPTEN_KEEPALIVE int web_load_state() {
    return load_state.load();
}

} // extern "C"

int main() {
    audio_ring.capacity = AUDIO_FRAMES;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    // canvas の制御をエミュレーションスレッドへ移す（WebGL2 / 2D コンテキストはそこで作る）
    emscripten_pthread_attr_settransferredcanvases(&attr, "#canvas");
    const int err = pthread_create(&emu_thread, &attr, EmuThreadMain, nullptr);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        std::fprintf(stderr, "エミュレーションスレッドを作成できません (%d)\n", err);
        return 1;
    }
    return 0;
}
