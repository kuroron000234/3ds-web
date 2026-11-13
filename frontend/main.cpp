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
#include <exception>
#include <new>
#include <typeinfo>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

// ROM 読み込みスレッドの準備: メインスレッドが BroadcastChannel で送ってくる File を受け取る。
// File はメモリに載せず、読むときに必要な範囲だけ FileReaderSync で取り出す。
EM_JS(void, RomChannelInit, (), {
    Module.__roms = {};
    const ch = Module.__romChannel = new BroadcastChannel('3ds-web-rom');
    ch.onmessage = (e) => {
        const m = e.data;
        if (m && m.file && m.id) {
            Module.__roms[m.id] = m.file;
            ch.postMessage({ack: m.id});
        }
    };
});

// ROM 読み込みスレッドで実行: File の [offset, offset+length) を dst へ読む。読めた長さか -1。
EM_JS(int, RomReadRange, (int id, double offset, void* dst, int length), {
    try {
        const file = Module.__roms[id];
        if (!file) return -1;
        const reader = Module.__romReader || (Module.__romReader = new FileReaderSync());
        const bytes = new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length)));
        HEAPU8.set(bytes, dst);
        return bytes.length;
    } catch (e) {
        err('ROM を読めません: ' + e);
        return -1;
    }
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
    // コアはログのフィルタを Debug に固定していて、シェーダーのコンパイルやメモリの割り当てごとに
    // 1 行出す。stderr への書き込みはメインスレッドへ同期で委譲される（終わるまで待つ）ので、
    // 遊んでいる間に何千行も流すと遅くなる。デバッグログは捨てる
    if (level < RETRO_LOG_INFO) {
        return;
    }
    static const char* const names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[%s] ", names[level <= RETRO_LOG_ERROR ? level : RETRO_LOG_ERROR]);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

// ---- ROM の仮想ファイル: ROM だけはブラウザの File から直接読む ----
// Emscripten の仮想ファイルシステムに載せると ROM 全体がメモリに乗り、2GB 前後で読めなくなる。
// そこで JS が web_mount_rom で登録したパスを Azahar が開くとき（common/file_util.cpp の
// IOFile が web_rom_fopen / web_rom_pread を呼ぶ）、読み込みを ROM 読み込みスレッド
// （FileReaderSync が使えるワーカー）へ委譲する。ブロック単位でキャッシュするので、細かい
// 読み込みは委譲しない。それ以外のパス（セーブデータなど）は通常の stdio のまま。
pthread_t rom_thread{};

constexpr uint32_t ROM_BLOCK = 256 * 1024;
constexpr size_t ROM_CACHE_BLOCKS = 96; // 24MB

struct RomSource {
    int id;
    uint64_t size;
};

struct RomHandle {
    std::string path;
    RomSource source;
    uint64_t pos = 0;
};

struct RomBlock {
    uint64_t key; // (id << 40) | ブロック番号
    std::vector<uint8_t> data;
};

std::mutex rom_mutex; // 以下すべてを守る（読み込みも直列にする）
std::map<std::string, RomSource> rom_sources;
std::list<RomBlock> rom_cache; // 先頭が最近使ったもの
std::unordered_map<uint64_t, std::list<RomBlock>::iterator> rom_cache_index;

struct RomReadRequest {
    int id;
    double offset;
    void* dst;
    int length;
    int result;
};

// rom_mutex を持った状態で呼ぶ。ブロックを返す（読めなければ nullptr）。
const std::vector<uint8_t>* RomGetBlock(const RomSource& src, uint64_t block) {
    const uint64_t key = (static_cast<uint64_t>(src.id) << 40) | block;
    if (const auto it = rom_cache_index.find(key); it != rom_cache_index.end()) {
        rom_cache.splice(rom_cache.begin(), rom_cache, it->second);
        return &it->second->data;
    }
    const uint64_t offset = block * ROM_BLOCK;
    const uint64_t remain = src.size - offset;
    RomBlock entry{key, std::vector<uint8_t>(remain < ROM_BLOCK ? remain : ROM_BLOCK)};
    RomReadRequest req{src.id, static_cast<double>(offset), entry.data.data(),
                       static_cast<int>(entry.data.size()), -1};
    const auto run = [](void* arg) {
        auto* r = static_cast<RomReadRequest*>(arg);
        r->result = RomReadRange(r->id, r->offset, r->dst, r->length);
    };
    if (pthread_equal(pthread_self(), rom_thread)) {
        run(&req);
    } else if (!emscripten_proxy_sync(emscripten_proxy_get_system_queue(), rom_thread, run, &req)) {
        return nullptr;
    }
    if (req.result != static_cast<int>(entry.data.size())) {
        return nullptr;
    }
    if (rom_cache.size() >= ROM_CACHE_BLOCKS) {
        rom_cache_index.erase(rom_cache.back().key);
        rom_cache.pop_back();
    }
    rom_cache.push_front(std::move(entry));
    rom_cache_index[key] = rom_cache.begin();
    return &rom_cache.front().data;
}

// rom_mutex を持った状態で呼ぶ。pos から len バイトを読む。読めた長さか -1。
int64_t RomReadAt(const RomSource& src, uint64_t pos, void* out, uint64_t len) {
    auto* dst = static_cast<uint8_t*>(out);
    uint64_t done = 0;
    while (done < len && pos < src.size) {
        const uint32_t in_block = static_cast<uint32_t>(pos % ROM_BLOCK);
        const auto* data = RomGetBlock(src, pos / ROM_BLOCK);
        if (!data) {
            return done ? static_cast<int64_t>(done) : -1;
        }
        uint64_t n = data->size() - in_block;
        if (n > len - done) {
            n = len - done;
        }
        std::memcpy(dst + done, data->data() + in_block, n);
        done += n;
        pos += n;
    }
    return static_cast<int64_t>(done);
}

// fopencookie 用のコールバック（stdio が FILE* の読み込み・シークをここへ回す）
ssize_t CookieRead(void* cookie, char* buf, size_t size) {
    auto* rom = static_cast<RomHandle*>(cookie);
    std::scoped_lock lock(rom_mutex);
    const int64_t n = RomReadAt(rom->source, rom->pos, buf, size);
    if (n > 0) {
        rom->pos += static_cast<uint64_t>(n);
    }
    return static_cast<ssize_t>(n);
}

int CookieSeek(void* cookie, off_t* offset, int whence) {
    auto* rom = static_cast<RomHandle*>(cookie);
    int64_t base = 0;
    if (whence == SEEK_CUR) {
        base = static_cast<int64_t>(rom->pos);
    } else if (whence == SEEK_END) {
        base = static_cast<int64_t>(rom->source.size);
    }
    if (base + *offset < 0) {
        return -1;
    }
    rom->pos = static_cast<uint64_t>(base + *offset);
    *offset = static_cast<off_t>(rom->pos);
    return 0;
}

std::map<std::FILE*, RomHandle*> rom_files; // rom_mutex で守る

int CookieClose(void* cookie) {
    auto* rom = static_cast<RomHandle*>(cookie);
    std::scoped_lock lock(rom_mutex);
    std::erase_if(rom_files, [rom](const auto& kv) { return kv.second == rom; });
    delete rom;
    return 0;
}

void* RomThreadMain(void*) {
    RomChannelInit();
    // イベントループへ戻って待つ（File の受け取りと、委譲されてくる読み込みを処理する）
    emscripten_exit_with_live_runtime();
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
    // 頂点処理は GPU で行う（WebGL2 に無いジオメトリシェーダーの分は頂点シェーダーに組み込み済み）
    {"citra_use_hw_shader", "enabled"},
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

// コアが積算している内訳（ms）: [0] GPU コマンド処理 [1] 画面出力 [2] 音声。残りが CPU コア。
// 窓ごとに差分を取り、エミュレーションスレッドの経過時間に対する割合（%）にして公開する。
extern "C" double web_prof_ms[4];
double prof_last[3]{};
std::atomic<double> prof_percent[3]{};

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
        for (int i = 0; i < 3; ++i) {
            prof_percent[i].store((web_prof_ms[i] - prof_last[i]) / elapsed * 100.0,
                                  std::memory_order_relaxed);
            prof_last[i] = web_prof_ms[i];
        }
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

// コアが投げた C++ 例外を受け止めて、種類とメッセージをログに出す。受け止めないと
// "Uncaught WebAssembly.Exception" としか出ず、原因が分からないままスレッドが止まる。
// 例外のあとはコアの状態が壊れているかもしれないので、エミュレーションは止める。
template <typename F>
bool GuardCore(const char* what, F&& body) {
    try {
        body();
        return true;
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "[MSG] %s中にメモリが足りなくなりました (std::bad_alloc)\n", what);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[MSG] %s中に例外: %s: %s\n", what, typeid(e).name(), e.what());
    } catch (...) {
        std::fprintf(stderr, "[MSG] %s中に不明な例外\n", what);
    }
    std::fprintf(stderr, "[ERROR] 使用メモリ %.0fMB\n",
                 static_cast<double>(__builtin_wasm_memory_size(0)) * 65536 / 1048576);
    game_loaded = false;
    return false;
}

void LoadGameOnEmuThread(void* arg) {
    auto* req = static_cast<LoadRequest*>(arg);
    req->result = false;
    if (game_loaded) {
        retro_unload_game();
        game_loaded = false;
    }

    retro_game_info info{};
    info.path = req->path.c_str();
    bool loaded = false;
    if (!GuardCore("読み込み", [&] { loaded = retro_load_game(&info); })) {
        return;
    }
    if (!loaded) {
        std::fprintf(stderr, "retro_load_game に失敗: %s\n", req->path.c_str());
        return;
    }

    if (hw_render_set) {
        if (gl_context == 0 && !CreateGLContext()) {
            return;
        }
        // Azahar はここ（context_reset）で実際にゲームを読み込む
        if (hw_render.context_reset && !GuardCore("読み込み", hw_render.context_reset)) {
            return;
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
        if (!GuardCore("実行", RunFrame)) {
            return;
        }
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

// JS から呼ぶ: 直近約 1 秒の内訳（%）。0 = GPU コマンド処理、1 = 画面出力、2 = 音声。
// web_get_busy からこの 3 つを引いた残りが CPU コア（インタプリタと OS の HLE）。
EMSCRIPTEN_KEEPALIVE double web_get_prof(int slot) {
    return slot >= 0 && slot < 3 ? prof_percent[slot].load(std::memory_order_relaxed) : 0.0;
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

// JS から呼ぶ: ブラウザの File を仮想パスに結び付ける。id は BroadcastChannel '3ds-web-rom' で
// ROM 読み込みスレッドへ送った File の番号（ack を受け取ってから呼ぶ）。size は File のバイト数。
EMSCRIPTEN_KEEPALIVE void web_mount_rom(const char* path, int id, double size) {
    std::scoped_lock lock(rom_mutex);
    rom_sources[path] = RomSource{id, static_cast<uint64_t>(size)};
}

// Azahar（IOFile::Open）から呼ばれる: path が登録済みの ROM なら、それを読む FILE* を返す。
// ROM でなければ nullptr（呼び出し側が通常の fopen に進む）。
EMSCRIPTEN_KEEPALIVE std::FILE* web_rom_fopen(const char* path) {
    RomHandle* rom = nullptr;
    {
        std::scoped_lock lock(rom_mutex);
        const auto it = rom_sources.find(path);
        if (it == rom_sources.end()) {
            return nullptr;
        }
        rom = new RomHandle{path, it->second};
    }
    std::FILE* f = fopencookie(rom, "rb", {CookieRead, nullptr, CookieSeek, CookieClose});
    if (!f) {
        delete rom;
        return nullptr;
    }
    std::scoped_lock lock(rom_mutex);
    rom_files[f] = rom;
    return f;
}

// Azahar（IOFile::ReadAtImpl）から呼ばれる: ファイル位置を動かさずに offset から読む。
// f が ROM の FILE* でなければ -2、読めなければ -1。
EMSCRIPTEN_KEEPALIVE int64_t web_rom_pread(std::FILE* f, void* data, uint64_t count,
                                           uint64_t offset) {
    std::scoped_lock lock(rom_mutex);
    const auto it = rom_files.find(f);
    if (it == rom_files.end()) {
        return -2;
    }
    return RomReadAt(it->second->source, offset, data, count);
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
    if (pthread_create(&rom_thread, nullptr, RomThreadMain, nullptr) != 0) {
        std::fprintf(stderr, "ROM 読み込みスレッドを作成できません\n");
    }
    if (err != 0) {
        std::fprintf(stderr, "エミュレーションスレッドを作成できません (%d)\n", err);
        return 1;
    }
    return 0;
}
