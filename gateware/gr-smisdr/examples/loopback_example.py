#!/usr/bin/env python3
"""
loopback_example.py
--------------------
Minimalbeispiel: erzeugt ein Testsignal, encodiert es mit smisdr.encoder
in das In-Band-Protokoll, dekodiert es sofort wieder mit smisdr.decoder
und gibt sowohl das rekonstruierte I/Q-Signal als auch alle erkannten
R/S-Kommandos auf der Konsole aus.

Nach 2 Sekunden wird zusätzlich per Message live ein neuer NCO-Shift
injiziert (--> zeigt die Trigger-Funktionalität im laufenden Stream).

Aufruf:
    python3 loopback_example.py
"""

import time
import pmt
from gnuradio import gr, blocks, analog
from gnuradio import smisdr


class MsgPrinter(gr.sync_block):
    """Winziger Hilfsblock: gibt jede empfangene 'cmd'-Message aus."""

    def __init__(self):
        gr.sync_block.__init__(self, name="msg_printer", in_sig=None, out_sig=None)
        self.message_port_register_in(pmt.intern("cmd"))
        self.set_msg_handler(pmt.intern("cmd"), self.handle_msg)

    def handle_msg(self, msg):
        cmd = pmt.dict_ref(msg, pmt.intern("cmd"), pmt.PMT_NIL)
        raw = pmt.dict_ref(msg, pmt.intern("raw"), pmt.from_uint64(0))
        hz = pmt.dict_ref(msg, pmt.intern("hz"), pmt.from_double(0))
        print(f"[DECODER] Kommando empfangen: {pmt.symbol_to_string(cmd)} "
              f"raw=0x{pmt.to_uint64(raw):08X} -> {pmt.to_double(hz):.1f} Hz")


class TopBlock(gr.top_block):
    def __init__(self):
        gr.top_block.__init__(self, "smisdr_loopback")

        samp_rate = 250000

        src = analog.sig_source_c(samp_rate, analog.GR_COS_WAVE, 5000, 0.8, 0)
        head = blocks.head(gr.sizeof_gr_complex, samp_rate * 5)  # 5 Sekunden

        self.enc = smisdr.encoder(sample_rate=samp_rate, shift_hz=2.0e6,
                                   master_clock=50e6, scale=8191,
                                   inject_at_start=True)

        self.dec = smisdr.decoder(master_clock=50e6, scale=8191,
                                   cmd_timeout_words=1000)

        sink = blocks.null_sink(gr.sizeof_gr_complex)
        msg_printer = MsgPrinter()

        self.connect(src, head, self.enc, self.dec, sink)
        self.msg_connect(self.dec, "cmd", msg_printer, "cmd")


def main():
    tb = TopBlock()
    tb.start()

    time.sleep(2.0)
    print("[MAIN] Injiziere Live-Shift auf 3.5 MHz ...")
    tb.enc.set_shift(3.5e6)

    time.sleep(1.0)
    print("[MAIN] Injiziere Live-Rate-Wechsel auf 125 ksps ...")
    tb.enc.set_sample_rate(125000)

    tb.wait()


if __name__ == "__main__":
    main()
