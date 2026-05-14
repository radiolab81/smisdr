// g++ -O3 -march=armv8-a -mtune=cortex-a72 -mfpu=neon-fp-armv8 -mfloat-abi=hard IQ2RF_test.cpp -o IQ2RF_test -lliquid -lpthread
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cmath>
#include <complex>
#include <liquid/liquid.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sched.h>
#include <cstring>
#include <arm_neon.h>

// --- Konfiguration ---
#define IN_PORT 1235
#define OUT_PORT 1234
#define CTRL_PORT 5001
#define TARGET_RATE 5000000.0f
#define BUFFER_SIZE (4 * 1024 * 1024)
#define BLOCK_SIZE 1024
#define NCO_LUT_SIZE 4096

// --- Globale Zustände ---
std::atomic<float> input_sample_rate(1250000.0f);
std::atomic<float> center_freq(0.0f);
std::atomic<bool> stop(false);
std::atomic<int> output_bits(16);
std::atomic<float> current_bit_scale(32767.0f);

alignas(16) float nco_sin_lut[NCO_LUT_SIZE];
alignas(16) float nco_cos_lut[NCO_LUT_SIZE];
static uint32_t phase_acc = 0;

struct IQBuffer {
    std::vector<liquid_float_complex> data;
    size_t valid_samples = 0;
    bool ready = false;
};

IQBuffer iq_buf_a, iq_buf_b;
IQBuffer* active_iq_fill = &iq_buf_a;
IQBuffer* active_iq_process = nullptr;
std::mutex mtx_iq;
std::condition_variable cv_iq_ready;
std::condition_variable cv_iq_free;

void init_nco_luts() {
    for (int i = 0; i < NCO_LUT_SIZE; i++) {
        nco_sin_lut[i] = std::sin(2.0f * M_PI * i / NCO_LUT_SIZE);
        nco_cos_lut[i] = std::cos(2.0f * M_PI * i / NCO_LUT_SIZE);
    }
}

void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

// --- CONTROL THREAD ---
void control_thread() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(CTRL_PORT), .sin_addr = {INADDR_ANY}};
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    while (!stop) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;
        char cmd[64] = {0};
        read(client, cmd, sizeof(cmd) - 1);
        std::string s_cmd(cmd);
        if (s_cmd.find("srin ") == 0) input_sample_rate = std::stof(s_cmd.substr(5));
        else if (s_cmd.find("freq ") == 0) center_freq = std::stof(s_cmd.substr(5));
        else if (s_cmd.find("outwidth ") == 0) {
            int b = std::stoi(s_cmd.substr(9));
            if (b >= 2 && b <= 16) {
                output_bits = b;
                current_bit_scale = (float)((1 << (b - 1)) - 1);
            }
        }
        close(client);
    }
}

// --- INPUT & RESAMPLING THREAD (Lückenlose Übergabe) ---
void input_resample_thread() {
    pin_to_core(1);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(IN_PORT), .sin_addr = {INADDR_ANY}};
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 1);

    const size_t buffer_samples = BUFFER_SIZE / sizeof(liquid_float_complex);
    iq_buf_a.data.resize(buffer_samples);
    iq_buf_b.data.resize(buffer_samples);

    while (!stop) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        float current_sr = input_sample_rate.load();
        msresamp_crcf resamp = msresamp_crcf_create(TARGET_RATE / current_sr, 60.0f);

        std::vector<int16_t> read_buf(BLOCK_SIZE * 2);
        liquid_float_complex x[BLOCK_SIZE];
        size_t write_pos = 0;

        while (!stop) {
            if (std::abs(current_sr - input_sample_rate.load()) > 1.0f) {
                current_sr = input_sample_rate;
                msresamp_crcf_destroy(resamp);
                resamp = msresamp_crcf_create(TARGET_RATE / current_sr, 60.0f);
            }

            size_t bytes_to_read = BLOCK_SIZE * 4;
            size_t total_read = 0;
            bool connection_lost = false;
            while (total_read < bytes_to_read && !stop) {
                ssize_t n = read(client_fd, (char*)read_buf.data() + total_read, bytes_to_read - total_read);
                if (n <= 0) { connection_lost = true; break; }
                total_read += n;
            }
            if (connection_lost || stop) break;

            for (int i = 0; i < BLOCK_SIZE; i++) {
                x[i] = {(float)read_buf[2*i]/32768.0f, (float)read_buf[2*i+1]/32768.0f};
            }

            if (write_pos + (BLOCK_SIZE * 6) >= buffer_samples) {
                std::unique_lock<std::mutex> lock(mtx_iq);
                size_t to_process = write_pos & ~3;
                size_t remainder = write_pos - to_process;

                active_iq_fill->valid_samples = to_process;
                active_iq_fill->ready = true;
                active_iq_process = active_iq_fill;
                cv_iq_ready.notify_one();

                IQBuffer* next_buf = (active_iq_fill == &iq_buf_a) ? &iq_buf_b : &iq_buf_a;
                while(next_buf->ready && !stop) cv_iq_free.wait(lock);

                if (remainder > 0) {
                    std::memcpy(&next_buf->data[0], &active_iq_fill->data[to_process], remainder * sizeof(liquid_float_complex));
                }
                active_iq_fill = next_buf;
                write_pos = remainder;
            }

            unsigned int nw;
            msresamp_crcf_execute(resamp, x, BLOCK_SIZE, &active_iq_fill->data[write_pos], &nw);
            write_pos += nw;
        }
        msresamp_crcf_destroy(resamp);
        close(client_fd);
    }
    close(server_fd);
}

// --- MIXER & OUTPUT THREAD (Hardware Rounding) ---
void mixer_output_thread(std::string target_ip) {
    pin_to_core(2);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr = {.sin_family = AF_INET, .sin_port = htons(OUT_PORT)};
    inet_pton(AF_INET, target_ip.c_str(), &serv_addr.sin_addr);

    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int sndbuf = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    while (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        if (stop) return;
        usleep(100000);
    }

    std::vector<int16_t> net_buf16;

    while (!stop) {
        std::unique_lock<std::mutex> lock(mtx_iq);
        cv_iq_ready.wait(lock, [] { return active_iq_process != nullptr || stop; });
        if (stop) break;

        IQBuffer* buf = active_iq_process;
        active_iq_process = nullptr;
        lock.unlock();

        size_t samples = buf->valid_samples;
        float bitScale = current_bit_scale.load();
        uint32_t phase_inc = (uint32_t)((center_freq / TARGET_RATE) * 4294967296.0f);

        // Konstanten für NEON
        float32x4_t v_gain = vdupq_n_f32(bitScale * 0.98f); // 2% Headroom gegen Clipping
        float32x4_t v_offset = vdupq_n_f32(bitScale);
        float32x4_t v_zero = vdupq_n_f32(0.0f);
        float32x4_t v_upper_limit = vdupq_n_f32(bitScale * 2.0f);

        net_buf16.resize(samples);

        for (size_t i = 0; i < samples; i += 4) {
            uint32_t idx0 = (phase_acc >> 20) & 0xFFF; phase_acc += phase_inc;
            uint32_t idx1 = (phase_acc >> 20) & 0xFFF; phase_acc += phase_inc;
            uint32_t idx2 = (phase_acc >> 20) & 0xFFF; phase_acc += phase_inc;
            uint32_t idx3 = (phase_acc >> 20) & 0xFFF; phase_acc += phase_inc;

            float32x4_t v_c = {nco_cos_lut[idx0], nco_cos_lut[idx1], nco_cos_lut[idx2], nco_cos_lut[idx3]};
            float32x4_t v_s = {nco_sin_lut[idx0], nco_sin_lut[idx1], nco_sin_lut[idx2], nco_sin_lut[idx3]};

            float32x4x2_t v_iq = vld2q_f32((float*)&buf->data[i]);

            float32x4_t v_res = vsubq_f32(vmulq_f32(v_iq.val[0], v_c), vmulq_f32(v_iq.val[1], v_s));
            v_res = vmlaq_f32(v_offset, v_res, v_gain);
            v_res = vminq_f32(vmaxq_f32(v_res, v_zero), v_upper_limit);

            // vcvtaq_s32_f32 = Hardware-Rundung (Round to nearest)
            //vst1_s16(&net_buf16[i], vqmovn_s32(vcvtaq_s32_f32(v_res)));
            float32x4_t v_rounded = vaddq_f32(v_res, vdupq_n_f32(0.5f));
            vst1_s16(&net_buf16[i], vqmovn_s32(vcvtq_s32_f32(v_rounded)));
        }

        send(sock, net_buf16.data(), samples * 2, 0);

        lock.lock();
        buf->ready = false;
        cv_iq_free.notify_one();
    }
    if (sock >= 0) close(sock);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Nutzung: ./IQ2RF <Ziel-IP>" << std::endl;
        return 1;
    }
    std::string target_ip = argv[1];
    init_nco_luts();

    std::thread t1(input_resample_thread);
    std::thread t2(mixer_output_thread, target_ip);
    std::thread t3(control_thread);

    if(t1.joinable()) t1.join();
    if(t2.joinable()) t2.join();
    if(t3.joinable()) t3.join();

    return 0;
}