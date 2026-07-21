/* -*- c++ -*- */
#ifndef INCLUDED_SMISDR_DECODER_H
#define INCLUDED_SMISDR_DECODER_H

#include <gnuradio/block.h>
#include <gnuradio/smisdr/api.h>

namespace gr {
namespace smisdr {

/*!
 * \brief Dekodiert das smiSDR 16-Bit In-Band-Signaling-Protokoll
 * zurück in einen komplexen I/Q-Strom und extrahiert Konfigurations-
 * kommandos ('R'/'S') als Nachrichten.
 *
 * Eingang:  1x short    (16-Bit Protokollwörter, z.B. aus File/TCP Source)
 * Ausgang:  1x complex  (rekonstruierte, normalisierte I/Q-Samples)
 * Message-Port "cmd" (Ausgang): PMT-Dict pro erfolgreich empfangenem
 *   Kommando: {"cmd": "R"|"S", "raw": u32-Tuning-Word, "hz": double}
 *
 * Die State Machine bildet exakt das Verhalten von smi_rx_16bit.v ab,
 * inklusive der Eigenheit, dass ein Command-End nur committet, was
 * bis dahin im Parameterpuffer steht (keine Vollständigkeitsprüfung
 * der 4 Bytes) — kompatibel zur Hardware.
 *
 * \ingroup smisdr
 */
class SMISDR_API decoder : virtual public gr::block
{
public:
    typedef std::shared_ptr<decoder> sptr;

    /*!
     * \param master_clock   FPGA-Referenztakt zur Rückrechnung des
     *                       32-Bit Tuning Words in Hz (Default 50 MHz).
     * \param scale          Vollausschlag zur Rückskalierung der 14-Bit
     *                       I/Q-Werte auf normalisierten complex-Output
     *                       (Default 8191, muss zum Encoder passen).
     * \param cmd_timeout_words  Watchdog: Anzahl empfangener Wörter ohne
     *                       abgeschlossenes Kommando, nach der der
     *                       Command-State zwangsweise auf IDLE
     *                       zurückgesetzt wird (Software-Äquivalent zu
     *                       TIMEOUT_CYCLES in smi_rx_16bit.v). 0 = aus.
     */
    static sptr make(double master_clock = 50e6,
                      int scale = 8191,
                      unsigned int cmd_timeout_words = 0);
};

} // namespace smisdr
} // namespace gr

#endif /* INCLUDED_SMISDR_DECODER_H */
