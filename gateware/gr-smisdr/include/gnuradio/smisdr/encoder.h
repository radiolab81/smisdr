/* -*- c++ -*- */
#ifndef INCLUDED_SMISDR_ENCODER_H
#define INCLUDED_SMISDR_ENCODER_H

#include <gnuradio/block.h>
#include <gnuradio/smisdr/api.h>

namespace gr {
namespace smisdr {

/*!
 * \brief Encodiert einen komplexen I/Q-Strom in das smiSDR
 * 16-Bit In-Band-Signaling-Protokoll (smiBus / ESP32 PARLIO).
 *
 * Eingang:  1x complex   (normalisiert, |I|,|Q| <= 1.0)
 * Ausgang:  1x short     (16-Bit Protokollwörter, 2 pro I/Q-Sample)
 *
 * Wortformat (siehe gateware/README.md, verifiziert gegen smi_rx_16bit.v):
 *
 *   Bit 15  Bit 14  Bits[13:8]        Bits[7:0]
 *   ------  ------  ----------        ---------
 *     0       0     14-Bit I-Daten (Bit 13 = Vorzeichen der 14-Bit-Zahl)
 *     0       1     14-Bit Q-Daten
 *     1       0     Index (0..3 = Byte-Position) oder 63 = Command-Init
 *     1       1     ignoriert         ASCII 'E' = Command-End/Execute
 *
 * Die Reihenfolge I->Q pro Sample ist zwingend (Phasenausrichtung in
 * der Hardware). Konfigurationskommandos ('R' = Rate, 'S' = Shift)
 * werden als 6-Wort-Sequenz (Init, 4x Param-Byte LE, End) VOR das
 * nächste I/Q-Paar eingeschoben, sobald der Ausgabepuffer Platz hat.
 *
 * \ingroup smisdr
 */
class SMISDR_API encoder : virtual public gr::block
{
public:
    typedef std::shared_ptr<encoder> sptr;

    /*!
     * \param sample_rate      Eingangs-I/Q-Samplerate in Hz. Wird als
     *                         'R'-Kommando (32-Bit Tuning Word) codiert.
     * \param shift_hz         Initialer NCO-Frequenzshift in Hz. Wird
     *                         als 'S'-Kommando codiert.
     * \param master_clock     FPGA-Referenztakt für die Tuning-Word-
     *                         Berechnung (Default 50 MHz, siehe sim_main.cpp).
     * \param scale            Vollausschlag zur Umrechnung von
     *                         normalisiertem complex-Input auf 14-Bit
     *                         signed (Default 8191 = 2^13 - 1).
     * \param inject_at_start  Wenn true, wird die R/S-Kommandosequenz
     *                         vor dem allerersten I/Q-Sample gesendet
     *                         (Standardverhalten, analog sim_main.cpp).
     */
    static sptr make(double sample_rate,
                      double shift_hz,
                      double master_clock = 50e6,
                      int scale = 8191,
                      bool inject_at_start = true);

    /*!
     * \brief Fügt zur Laufzeit ein neues 'S'-Kommando (NCO-Shift) in
     * den ausgehenden Stream ein. Threadsicher, kann aus einem anderen
     * Thread oder via Message-Port "cmd" ausgelöst werden.
     */
    virtual void set_shift(double shift_hz) = 0;

    /*!
     * \brief Fügt zur Laufzeit ein neues 'R'-Kommando (Samplerate) in
     * den ausgehenden Stream ein.
     */
    virtual void set_sample_rate(double sample_rate) = 0;
};

} // namespace smisdr
} // namespace gr

#endif /* INCLUDED_SMISDR_ENCODER_H */
