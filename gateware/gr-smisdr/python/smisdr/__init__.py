#
# gnuradio.smisdr — Python-Schnittstelle des smiSDR In-Band-Signaling
# OOT-Moduls für GNU Radio 3.10.
#
# Stellt zwei Blöcke bereit:
#   smisdr.encoder(sample_rate, shift_hz, master_clock=50e6,
#                   scale=8191, inject_at_start=True)
#   smisdr.decoder(master_clock=50e6, scale=8191, cmd_timeout_words=0)
#

import os

# Importiert die kompilierten pybind11-Bindings (smisdr_python.*.so)
try:
    from .smisdr_python import *
except ImportError:
    dirname, filename = os.path.split(os.path.abspath(__file__))
    __path__.append(os.path.join(dirname, "bindings"))
    from .smisdr_python import *
