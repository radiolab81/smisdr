/* -*- c++ -*- */
#include "encoder_impl.h"

#include <gnuradio/io_signature.h>
#include <pmt/pmt.h>

#include <algorithm>
#include <cmath>

namespace gr {
namespace smisdr {

// ---------------------------------------------------------------------
// Protokollkonstanten (verifiziert gegen smi_rx_16bit.v)
// ---------------------------------------------------------------------
namespace {
constexpr uint8_t CTRL_I = 0b00;
constexpr uint8_t CTRL_Q = 0b01;
constexpr uint8_t CTRL_PARAM = 0b10;
constexpr uint8_t CTRL_END = 0b11;
constexpr uint8_t IDX_INIT = 0x3F; // 63 = "Command Init" / Sync-Marker
constexpr uint8_t END_PAYLOAD = 'E';
} // namespace

encoder::sptr encoder::make(double sample_rate,
                             double shift_hz,
                             double master_clock,
                             int scale,
                             bool inject_at_start)
{
    return gnuradio::make_block_sptr<encoder_impl>(
        sample_rate, shift_hz, master_clock, scale, inject_at_start);
}

encoder_impl::encoder_impl(double sample_rate,
                            double shift_hz,
                            double master_clock,
                            int scale,
                            bool inject_at_start)
    : gr::block("smisdr_encoder",
                gr::io_signature::make(1, 1, sizeof(gr_complex)),
                gr::io_signature::make(1, 1, sizeof(short))),
      d_master_clock(master_clock),
      d_scale(scale)
{
    message_port_register_in(pmt::mp("cmd"));
    set_msg_handler(pmt::mp("cmd"), [this](pmt::pmt_t msg) { handle_cmd_message(msg); });

    if (inject_at_start) {
        // Reihenfolge wie in sim_main.cpp: erst 'S' (Shift), dann 'R' (Rate).
        // Beide Reihenfolgen sind für die Hardware äquivalent, wir bleiben
        // aber konsistent zur Referenz-Testbench.
        queue_command('S', calc_ftw(shift_hz, d_master_clock));
        queue_command('R', calc_ftw(sample_rate, d_master_clock));
    }
}

encoder_impl::~encoder_impl() {}

uint32_t encoder_impl::calc_ftw(double freq_hz, double master_clock)
{
    double tw = (freq_hz / master_clock) * 4294967296.0; // 2^32
    return static_cast<uint32_t>(std::llround(tw));
}

uint16_t encoder_impl::make_word(uint8_t ctrl, uint8_t idx, uint8_t payload)
{
    return (static_cast<uint16_t>(ctrl & 0x03) << 14) |
           (static_cast<uint16_t>(idx & 0x3F) << 8) | payload;
}

void encoder_impl::queue_command(char cmd_char, uint32_t value)
{
    std::lock_guard<std::mutex> lock(d_cmd_mutex);
    // 1) Command Init: ctrl=10, idx=63 (Sync/Reset des Empfänger-Parsers), Payload = ASCII-Befehl
    d_cmd_fifo.push_back(make_word(CTRL_PARAM, IDX_INIT, static_cast<uint8_t>(cmd_char)));
    // 2) 4x Param Chunk, Little Endian, ctrl=10, idx=0..3
    d_cmd_fifo.push_back(make_word(CTRL_PARAM, 0, (value >> 0) & 0xFF));
    d_cmd_fifo.push_back(make_word(CTRL_PARAM, 1, (value >> 8) & 0xFF));
    d_cmd_fifo.push_back(make_word(CTRL_PARAM, 2, (value >> 16) & 0xFF));
    d_cmd_fifo.push_back(make_word(CTRL_PARAM, 3, (value >> 24) & 0xFF));
    // 3) Command End: ctrl=11, Payload = 'E' -> löst das Commit in der Hardware aus
    d_cmd_fifo.push_back(make_word(CTRL_END, 0, END_PAYLOAD));
}

void encoder_impl::set_shift(double shift_hz)
{
    queue_command('S', calc_ftw(shift_hz, d_master_clock));
}

void encoder_impl::set_sample_rate(double sample_rate)
{
    queue_command('R', calc_ftw(sample_rate, d_master_clock));
}

void encoder_impl::handle_cmd_message(pmt::pmt_t msg)
{
    // Erwartetes Format: PMT-Pair (Symbol . Double), z.B. aus GRC via
    // "Message Strobe" + "PDU Set"/eigenem Python-Block erzeugt:
    //   pmt.cons(pmt.intern("shift"), pmt.from_double(2.0e6))
    //   pmt.cons(pmt.intern("rate"),  pmt.from_double(500000.0))
    // Alternativ ein Dict mit denselben Keys wird ebenfalls akzeptiert.
    pmt::pmt_t key_pmt, val_pmt;

    if (pmt::is_pair(msg)) {
        key_pmt = pmt::car(msg);
        val_pmt = pmt::cdr(msg);
    } else if (pmt::is_dict(msg)) {
        pmt::pmt_t keys = pmt::dict_keys(msg);
        if (pmt::length(keys) < 1)
            return;
        key_pmt = pmt::nth(0, keys);
        val_pmt = pmt::dict_ref(msg, key_pmt, pmt::PMT_NIL);
    } else {
        GR_LOG_WARN(d_logger, "smisdr_encoder: unbekanntes Nachrichtenformat auf 'cmd' ignoriert");
        return;
    }

    if (!pmt::is_symbol(key_pmt) || !pmt::is_number(val_pmt)) {
        GR_LOG_WARN(d_logger, "smisdr_encoder: 'cmd' Nachricht muss (symbol . number) sein");
        return;
    }

    const std::string key = pmt::symbol_to_string(key_pmt);
    const double value = pmt::to_double(val_pmt);

    if (key == "shift") {
        set_shift(value);
    } else if (key == "rate") {
        set_sample_rate(value);
    } else {
        GR_LOG_WARN(d_logger, "smisdr_encoder: unbekannter Kommando-Key '" + key + "' (erwartet 'shift' oder 'rate')");
    }
}

void encoder_impl::forecast(int noutput_items, gr_vector_int& ninput_items_required)
{
    // Worst Case: kein Kommando anstehend -> 2 Ausgabewörter pro Eingangssample.
    // Ausreichend Puffer, tatsächlicher Verbrauch wird in general_work exakt berechnet.
    ninput_items_required[0] = noutput_items / 2 + 1;
}

int encoder_impl::general_work(int noutput_items,
                                gr_vector_int& ninput_items,
                                gr_vector_const_void_star& input_items,
                                gr_vector_void_star& output_items)
{
    const gr_complex* in = reinterpret_cast<const gr_complex*>(input_items[0]);
    short* out = reinterpret_cast<short*>(output_items[0]);

    int produced = 0;
    int consumed = 0;
    const int n_in = ninput_items[0];

    while (produced < noutput_items) {
        // 1) Anstehende In-Band-Kommandos haben Vorrang und werden Wort für
        //    Wort ausgegeben, bevor das nächste I/Q-Paar folgt.
        {
            std::lock_guard<std::mutex> lock(d_cmd_mutex);
            if (!d_cmd_fifo.empty()) {
                out[produced++] = static_cast<short>(d_cmd_fifo.front());
                d_cmd_fifo.pop_front();
                continue;
            }
        }

        // 2) Ein I/Q-Paar braucht 2 freie Ausgabeplätze UND 1 Eingangssample.
        if (produced + 2 > noutput_items)
            break;
        if (consumed >= n_in)
            break;

        const gr_complex s = in[consumed++];

        // Clipping auf +/-1.0, dann Skalierung auf 14-Bit signed (-scale..+scale)
        float re = std::max(-1.0f, std::min(1.0f, s.real()));
        float im = std::max(-1.0f, std::min(1.0f, s.imag()));

        int32_t i_raw = static_cast<int32_t>(std::lround(re * d_scale));
        int32_t q_raw = static_cast<int32_t>(std::lround(im * d_scale));

        // I-Wort: ctrl=00 ergibt sich implizit, da die Maskierung auf 14 Bit
        // die oberen 2 Bit auf 0 setzt.
        uint16_t i_word = static_cast<uint16_t>(i_raw & 0x3FFF);
        // Q-Wort: ctrl=01 -> Bit 14 explizit setzen.
        uint16_t q_word = static_cast<uint16_t>((q_raw & 0x3FFF) | (1 << 14));

        out[produced++] = static_cast<short>(i_word);
        out[produced++] = static_cast<short>(q_word);
    }

    consume(0, consumed);
    return produced;
}

} // namespace smisdr
} // namespace gr
