/* -*- c++ -*- */
#include "decoder_impl.h"

#include <gnuradio/io_signature.h>
#include <pmt/pmt.h>

namespace gr {
namespace smisdr {

namespace {
constexpr uint8_t CTRL_I = 0b00;
constexpr uint8_t CTRL_Q = 0b01;
constexpr uint8_t CTRL_PARAM = 0b10;
constexpr uint8_t CTRL_END = 0b11;
constexpr uint8_t IDX_INIT = 0x3F;
constexpr uint8_t END_PAYLOAD = 'E';
} // namespace

decoder::sptr decoder::make(double master_clock, int scale, unsigned int cmd_timeout_words)
{
    return gnuradio::make_block_sptr<decoder_impl>(master_clock, scale, cmd_timeout_words);
}

decoder_impl::decoder_impl(double master_clock, int scale, unsigned int cmd_timeout_words)
    : gr::block("smisdr_decoder",
                gr::io_signature::make(1, 1, sizeof(short)),
                gr::io_signature::make(1, 1, sizeof(gr_complex))),
      d_master_clock(master_clock),
      d_scale(scale),
      d_cmd_timeout_words(cmd_timeout_words),
      d_cmd_state(CMD_IDLE),
      d_active_cmd(0),
      d_param_buf(0),
      d_i_latch(0),
      d_watchdog(0)
{
    message_port_register_out(pmt::mp("cmd"));
}

decoder_impl::~decoder_impl() {}

int16_t decoder_impl::sign_extend14(uint16_t word)
{
    // Untere 14 Bit extrahieren, Bit 13 als Vorzeichen nach oben durchziehen
    // -- identisch zu $signed({ {2{data_sync[13]}}, data_sync[13:0] }) in HDL.
    uint16_t v = word & 0x3FFF;
    if (v & 0x2000)
        v |= 0xC000; // obere 2 Bit auf 1 setzen -> negative Zahl in int16_t
    return static_cast<int16_t>(v);
}

double decoder_impl::ftw_to_hz(uint32_t ftw) const
{
    return (static_cast<double>(ftw) / 4294967296.0) * d_master_clock; // 2^32
}

void decoder_impl::publish_cmd(uint8_t cmd_char, uint32_t raw_value)
{
    pmt::pmt_t dict = pmt::make_dict();
    dict = pmt::dict_add(dict, pmt::mp("cmd"), pmt::mp(std::string(1, static_cast<char>(cmd_char))));
    dict = pmt::dict_add(dict, pmt::mp("raw"), pmt::from_uint64(raw_value));
    dict = pmt::dict_add(dict, pmt::mp("hz"), pmt::from_double(ftw_to_hz(raw_value)));
    message_port_pub(pmt::mp("cmd"), dict);
}

void decoder_impl::forecast(int noutput_items, gr_vector_int& ninput_items_required)
{
    // Worst Case (keine Kommandos im Stream): 2 Eingangswörter pro Ausgabesample.
    ninput_items_required[0] = noutput_items * 2;
}

int decoder_impl::general_work(int noutput_items,
                                gr_vector_int& ninput_items,
                                gr_vector_const_void_star& input_items,
                                gr_vector_void_star& output_items)
{
    const short* in = reinterpret_cast<const short*>(input_items[0]);
    gr_complex* out = reinterpret_cast<gr_complex*>(output_items[0]);

    const int n_in = ninput_items[0];
    int consumed = 0;
    int produced = 0;

    while (consumed < n_in && produced < noutput_items) {
        const uint16_t w = static_cast<uint16_t>(in[consumed++]);
        const uint8_t ctrl = (w >> 14) & 0x3;
        const uint8_t idx = (w >> 8) & 0x3F;
        const uint8_t payload = w & 0xFF;

        // Watchdog: mirrors TIMEOUT_CYCLES in smi_rx_16bit.v (dort takt-, hier
        // wortbasiert). Nur relevant, wenn cmd_timeout_words > 0 konfiguriert ist.
        if (d_cmd_state != CMD_IDLE) {
            if (d_cmd_timeout_words > 0) {
                d_watchdog++;
                if (d_watchdog >= d_cmd_timeout_words) {
                    d_cmd_state = CMD_IDLE;
                    d_watchdog = 0;
                }
            }
        } else {
            d_watchdog = 0;
        }

        switch (ctrl) {
        case CTRL_I:
            d_i_latch = sign_extend14(w);
            break;

        case CTRL_Q: {
            const int16_t q = sign_extend14(w);
            out[produced++] = gr_complex(static_cast<float>(d_i_latch) / d_scale,
                                          static_cast<float>(q) / d_scale);
            break;
        }

        case CTRL_PARAM:
            d_watchdog = 0; // jede gültige Command-Aktivität setzt den Watchdog zurück
            if (idx == IDX_INIT) {
                d_active_cmd = payload;
                d_param_buf = 0;
                d_cmd_state = CMD_RECV;
            } else if (d_cmd_state == CMD_RECV) {
                switch (idx) {
                case 0:
                    d_param_buf = (d_param_buf & 0xFFFFFF00u) | payload;
                    break;
                case 1:
                    d_param_buf = (d_param_buf & 0xFFFF00FFu) | (static_cast<uint32_t>(payload) << 8);
                    break;
                case 2:
                    d_param_buf = (d_param_buf & 0xFF00FFFFu) | (static_cast<uint32_t>(payload) << 16);
                    break;
                case 3:
                    d_param_buf = (d_param_buf & 0x00FFFFFFu) | (static_cast<uint32_t>(payload) << 24);
                    break;
                default:
                    // Indizes > 3 werden wie in der Hardware stillschweigend ignoriert
                    break;
                }
            }
            break;

        case CTRL_END:
            if (d_cmd_state == CMD_RECV && payload == END_PAYLOAD) {
                // kompatibel zur Hardware: es wird NICHT geprüft, ob
                // tatsächlich alle 4 Parameter-Bytes empfangen wurden.
                publish_cmd(d_active_cmd, d_param_buf);
                d_cmd_state = CMD_IDLE;
            }
            break;
        }
    }

    consume(0, consumed);
    return produced;
}

} // namespace smisdr
} // namespace gr
