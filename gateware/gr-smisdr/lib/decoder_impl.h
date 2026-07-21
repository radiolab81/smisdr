/* -*- c++ -*- */
#ifndef INCLUDED_SMISDR_DECODER_IMPL_H
#define INCLUDED_SMISDR_DECODER_IMPL_H

#include <gnuradio/smisdr/decoder.h>

#include <cstdint>

namespace gr {
namespace smisdr {

class decoder_impl : public decoder
{
private:
    double d_master_clock;
    int d_scale;
    unsigned int d_cmd_timeout_words;

    // --- Spiegelt exakt den Zustand der Verilog State Machine ---
    enum cmd_state_t { CMD_IDLE = 0, CMD_RECV = 1 };
    cmd_state_t d_cmd_state;
    uint8_t d_active_cmd;   // 'R' oder 'S'
    uint32_t d_param_buf;   // wird Byte fuer Byte (LE) zusammengebaut
    int16_t d_i_latch;      // gelatchtes I-Sample, bis Q eintrifft
    unsigned int d_watchdog;

    static int16_t sign_extend14(uint16_t word);
    double ftw_to_hz(uint32_t ftw) const;
    void publish_cmd(uint8_t cmd_char, uint32_t raw_value);

public:
    decoder_impl(double master_clock, int scale, unsigned int cmd_timeout_words);
    ~decoder_impl() override;

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;

    int general_work(int noutput_items,
                      gr_vector_int& ninput_items,
                      gr_vector_const_void_star& input_items,
                      gr_vector_void_star& output_items) override;
};

} // namespace smisdr
} // namespace gr

#endif /* INCLUDED_SMISDR_DECODER_IMPL_H */
