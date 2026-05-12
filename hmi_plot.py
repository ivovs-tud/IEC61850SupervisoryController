#!/usr/bin/env python3
"""
Wind Farm HMI – real-time time-series plots.

Subscribes to the supervisory controller's ZMQ PUB socket (IPC endpoint by
default), receives msgpack snapshots, and renders them with pyqtgraph for
smooth, flicker-free incremental updates.

Dependencies:
    pip install pyzmq msgpack pyqtgraph pyqt5

Usage:
    python hmi_plot.py
    python hmi_plot.py tcp://localhost:9004
    python hmi_plot.py ipc:///tmp/supervisory_controller_hmi.sock   # default
"""

import sys
from collections import deque

import msgpack
import pyqtgraph as pg
import zmq
from pyqtgraph.Qt import QtCore, QtWidgets

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
if len(sys.argv) > 1:
    ENDPOINT = sys.argv[1] 
else:
    if sys.platform == "win32":  # covers both 32 and 64-bit Windows
        ENDPOINT = "tcp://localhost:5555"
    else:
        ENDPOINT = "ipc:///tmp/supervisory_controller_hmi.sock"
#"ipc:///tmp/supervisory_controller_hmi.sock"

POLL_INTERVAL_MS = 50  # how often the Qt timer checks for new ZMQ messages
LAYOUT_ROWS = 3
LAYOUT_COLS = 2
MAX_PLOTS = LAYOUT_ROWS * LAYOUT_COLS

# Colour palette: one colour per line within a subplot.
COLORS = [
    (31,  119, 180),  # blue
    (255, 127,  14),  # orange
    (44,  160,  44),  # green
    (214,  39,  40),  # red
    (148, 103, 189),  # purple
    (140,  86,  75),  # brown
    (227, 119, 194),  # pink
    (127, 127, 127),  # grey
]

# ---------------------------------------------------------------------------
# ZMQ subscriber (non-blocking)
# ---------------------------------------------------------------------------
ctx = zmq.Context()
sock = ctx.socket(zmq.SUB)
sock.connect(ENDPOINT)
sock.setsockopt(zmq.SUBSCRIBE, b"")
sock.setsockopt(zmq.RCVHWM, 5)   # drop stale frames if we fall behind

# ---------------------------------------------------------------------------
# pyqtgraph application
# ---------------------------------------------------------------------------
pg.setConfigOptions(antialias=True, foreground="w", background="k")
app = pg.mkQApp("Wind Farm HMI")

win = pg.GraphicsLayoutWidget(title=f"Wind Farm HMI  —  {ENDPOINT}")
win.resize(1280, 900)
win.show()

# ---------------------------------------------------------------------------
# State – initialised lazily from the first received message.
# ---------------------------------------------------------------------------
plots: list[pg.PlotItem] = []
curves: list[list[pg.PlotDataItem]] = []
histories: list[list[deque]] = []
window_size: int = 100
initialized: bool = False


def _qt_pen_style(name: str):
    """Return Qt pen style value for both Qt5 and Qt6 enum layouts."""
    # Qt6: QtCore.Qt.PenStyle.SolidLine / DashLine
    pen_style_enum = getattr(QtCore.Qt, "PenStyle", None)
    if pen_style_enum is not None and hasattr(pen_style_enum, name):
        return getattr(pen_style_enum, name)
    # Qt5: QtCore.Qt.SolidLine / DashLine
    return getattr(QtCore.Qt, name)


def init_layout(signals: list, ws: int) -> None:
    """Build all PlotItems and PlotDataItems from the first message."""
    global plots, curves, histories, window_size, initialized

    window_size = ws
    x_axis = list(range(window_size))

    for idx, (name, unit, labels, _values) in enumerate(signals[:MAX_PLOTS]):
        p: pg.PlotItem = win.addPlot(title=name)
        p.setLabel("left", unit)
        p.setLabel("bottom", "Sample  (newest → right)")
        p.showGrid(x=True, y=True, alpha=0.25)
        p.setXRange(0, window_size - 1, padding=0)
        legend = p.addLegend(offset=(10, 10))
        legend.setBrush(pg.mkBrush(0, 0, 0, 160))

        sig_curves: list[pg.PlotDataItem] = []
        sig_histories: list[deque] = []

        for j, label in enumerate(labels):
            color = COLORS[j % len(COLORS)]
            line_style = _qt_pen_style("SolidLine")
            if "setpoint" in str(label).lower():
                line_style = _qt_pen_style("DashLine")
            c = p.plot(
                x=x_axis,
                y=[0.0] * window_size,
                pen=pg.mkPen(color=color, width=2, style=line_style),
                name=label,
            )
            sig_curves.append(c)
            sig_histories.append(deque([0.0] * window_size, maxlen=window_size))

        plots.append(p)
        curves.append(sig_curves)
        histories.append(sig_histories)

        if (idx + 1) % LAYOUT_COLS == 0:
            win.nextRow()

    initialized = True


def poll_and_update() -> None:
    """Qt timer callback: drain the ZMQ socket and refresh plots."""
    global initialized

    # Drain up to 10 queued frames so we don't fall perpetually behind,
    # but always render only the latest one.
    raw = None
    for _ in range(10):
        try:
            raw = sock.recv(zmq.NOBLOCK)
        except zmq.Again:
            break   # nothing left in the queue

    if raw is None:
        return  # no new data this tick

    msg = msgpack.unpackb(raw, raw=False)
    tick, ws, signals = msg

    if not initialized:
        init_layout(signals, ws)

    x_axis = list(range(window_size))

    for i, (name, unit, labels, values) in enumerate(signals):
        if i >= len(curves):
            break
        for j, v in enumerate(values):
            if j >= len(curves[i]):
                break
            histories[i][j].append(float(v))
            curves[i][j].setData(x=x_axis, y=list(histories[i][j]))

    win.setWindowTitle(f"Wind Farm HMI  —  tick {tick}  —  {ENDPOINT}")


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
timer = QtCore.QTimer()
timer.timeout.connect(poll_and_update)
timer.start(POLL_INTERVAL_MS)

if __name__ == "__main__":
    QtWidgets.QApplication.instance().exec()
