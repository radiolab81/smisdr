/* -*- c++ -*- */
#ifndef INCLUDED_SMISDR_ENCODER_IMPL_H
#define INCLUDED_SMISDR_ENCODER_IMPL_H

#include <gnuradio/smisdr/encoder.h>

#include <cstdint>
#include <deque>
#include <mutex>

namespace gr {
namespace smisdr {

class encoder_impl : public encoder
{
private:
    double d_master_clock;
    int d_scale;

    // FIFO der noch zu sendenden 16-Bit In-Band-Kommandowörter.
    // Wird vor jedem I/Q-Paar zuerst geleert (hat Priorität), damit
    // Kommandos deterministisch VOR dem nächsten Sample-Paar liegen -
    // exakt wie in sim_main.cpp / cohi_wav_to_smi_iq.py.
    std::deque<uint16_t> d_cmd_fifo;
    std::mutex d_cmd_mutex;

    static uint32_t calc_ftw(double freq_hz, double master_clock);
    static uint16_t make_word(uint8_t ctrl, uint8_t idx, uint8_t payload);

    // Baut die vollständige 6-Wort Init/Param/End-Sequenz für ein
    // Kommando ('R' oder 'S') und hängt sie an die FIFO an.
    void queue_command(char cmd_char, uint32_t value);

    void handle_cmd_message(pmt::pmt_t msg);

public:
    encoder_impl(double sample_rate,
                 double shift_hz,
                 double master_clock,
                 int scale,
                 bool inject_at_start);
    ~encoder_impl() override;

    void set_shift(double shift_hz) override;
    void set_sample_rate(double sample_rate) override;

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;

    int general_work(int noutput_items,
                      gr_vector_int& ninput_items,
                      gr_vector_const_void_star& input_items,
                      gr_vector_void_star& output_items) override;
};

} // namespace smisdr
} // namespace gr

#endif /* INCLUDED_SMISDR_ENCODER_IMPL_H */
