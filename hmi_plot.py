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

import os
import sys
from collections import deque

# Prefer XCB unless the user already chose a Qt platform plugin.
#os.environ.setdefault("QT_QPA_PLATFORM", "xcb")

import msgpack
import pyqtgraph as pg
import zmq
from pyqtgraph.Qt import QtCore, QtGui, QtWidgets

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
if len(sys.argv) > 1:
    ENDPOINT = sys.argv[1] 
else:
    if sys.platform == "win32":  # covers both 32 and 64-bit Windows
        ENDPOINT = "tcp://localhost:5555"
        COMMAND_ENDPOINT = "tcp://localhost:5556"
    else:
        ENDPOINT = "ipc:///tmp/supervisory_controller_hmi.sock"
        COMMAND_ENDPOINT = sys.argv[2] if len(sys.argv) > 2 else "ipc:///tmp/supervisory_controller_hmi_cmd.sock"
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

cmd_sock = ctx.socket(zmq.PUSH)
cmd_sock.setsockopt(zmq.SNDHWM, 5)
cmd_sock.connect(COMMAND_ENDPOINT)

# ---------------------------------------------------------------------------
# pyqtgraph application
# ---------------------------------------------------------------------------
pg.setConfigOptions(antialias=True, foreground="w", background="#12161d")
app = pg.mkQApp("Wind Farm HMI")


def _qt_painter_hint(name: str):
    """Return Qt painter render hint for both Qt5 and Qt6 enum layouts."""
    # Qt6: QtGui.QPainter.RenderHint.Antialiasing
    render_hint_enum = getattr(QtGui.QPainter, "RenderHint", None)
    if render_hint_enum is not None and hasattr(render_hint_enum, name):
        return getattr(render_hint_enum, name)
    # Qt5: QtGui.QPainter.Antialiasing
    return getattr(QtGui.QPainter, name)


def _qt_pen_enum(name: str):
    """Return Qt pen style for both Qt5 and Qt6 enum layouts."""
    # Qt6: QtCore.Qt.PenStyle.NoPen
    pen_style_enum = getattr(QtCore.Qt, "PenStyle", None)
    if pen_style_enum is not None and hasattr(pen_style_enum, name):
        return getattr(pen_style_enum, name)
    # Qt5: QtCore.Qt.NoPen
    return getattr(QtCore.Qt, name)


class LedIndicator(QtWidgets.QWidget):
    class _LedFace(QtWidgets.QWidget):
        def __init__(self, color: tuple[int, int, int]):
            super().__init__()
            self.setFixedSize(32, 32)
            self._on = False
            self._color = color

        def set_on(self, state: bool) -> None:
            self._on = bool(state)
            self.update()

        def paintEvent(self, _event) -> None:
            p = QtGui.QPainter(self)
            try:
                p.setRenderHint(_qt_painter_hint("Antialiasing"), True)

                base_rect = self.rect().adjusted(2, 2, -2, -2)
                p.setPen(QtGui.QPen(QtGui.QColor("#20252d"), 2))

                cx = float(base_rect.center().x())
                cy = float(base_rect.center().y())

                if self._on:
                    r, g, b = self._color
                    glow = QtGui.QRadialGradient(cx, cy, 14.0)
                    glow.setColorAt(0.0, QtGui.QColor(r, g, b, 255))
                    glow.setColorAt(0.6, QtGui.QColor(r, g, b, 170))
                    glow.setColorAt(1.0, QtGui.QColor(20, 24, 30, 255))
                    p.setBrush(QtGui.QBrush(glow))
                else:
                    off_grad = QtGui.QRadialGradient(cx, cy, 14.0)
                    off_grad.setColorAt(0.0, QtGui.QColor("#616a78"))
                    off_grad.setColorAt(1.0, QtGui.QColor("#2a2f38"))
                    p.setBrush(QtGui.QBrush(off_grad))

                p.drawEllipse(base_rect)

                glare = QtCore.QRect(base_rect.left() + 7, base_rect.top() + 5, 9, 6)
                p.setBrush(QtGui.QColor(255, 255, 255, 110 if self._on else 55))
                p.setPen(_qt_pen_enum("NoPen"))
                p.drawEllipse(glare)
            finally:
                p.end()

    def __init__(self, label: str, color_name: str):
        super().__init__()
        self._label = QtWidgets.QLabel(label)
        self._label.setStyleSheet("color: #dce3ee; font-size: 13px; font-weight: 600;")

        self._color_map = {
            "red": (230, 54, 54),
            "green": (54, 186, 80),
            "amber": (240, 170, 34),
        }
        self._color = self._color_map.get(color_name.lower(), (230, 54, 54))

        self._face = self._LedFace(self._color)

        row = QtWidgets.QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(10)
        row.addWidget(self._face)
        row.addWidget(self._label)
        row.addStretch(1)
        self.setLayout(row)

    def set_state(self, is_on: bool) -> None:
        self._face.set_on(is_on)


class CircularModeButton(QtWidgets.QPushButton):
    def __init__(self, text: str, mode_index: int):
        super().__init__(text)
        self.mode_index = mode_index
        self.setCheckable(True)
        self.setMinimumSize(120, 120)
        self.setMaximumSize(120, 120)
        self.setStyleSheet(
            "QPushButton {"
            "border-radius: 60px;"
            "border: 3px solid #2a3340;"
            "background-color: #232a35;"
            "color: #f0f3f8;"
            "font-size: 15px;"
            "font-weight: 700;"
            "padding: 8px;"
            "}"
            "QPushButton:checked {"
            "background-color: #2f80ed;"
            "border: 3px solid #96c0ff;"
            "}"
            "QPushButton:hover {"
            "background-color: #2a3342;"
            "}"
            "QPushButton:pressed {"
            "background-color: #1d2430;"
            "}"
        )


class OffOnButton(QtWidgets.QPushButton):
    def __init__(self, off_text: str = "Off", on_text: str = "On", checked: bool = False):
        super().__init__(on_text if checked else off_text)
        self.off_text = str(off_text)
        self.on_text = str(on_text)
        self.setCheckable(True)
        self.setChecked(bool(checked))
        self.setMinimumSize(120, 60)
        self.setStyleSheet(
            "QPushButton {"
            "border-radius: 12px;"
            "border: 2px solid #2a3340;"
            "background-color: #232a35;"
            "color: #f0f3f8;"
            "font-size: 14px;"
            "font-weight: 700;"
            "padding: 10px;"
            "}"
            "QPushButton:checked {"
            "background-color: #2f80ed;"
            "border: 2px solid #96c0ff;"
            "}"
            "QPushButton:hover {"
            "background-color: #2a3342;"
            "}"
            "QPushButton:pressed {"
            "background-color: #1d2430;"
            "}"
        )
        self.toggled.connect(self._update_label)

    def _update_label(self, checked: bool) -> None:
        self.setText(self.on_text if checked else self.off_text)

    def set_state(self, is_on: bool) -> None:
        self.blockSignals(True)
        self.setChecked(bool(is_on))
        self._update_label(bool(is_on))
        self.blockSignals(False)


main_window = QtWidgets.QMainWindow()
main_window.setWindowTitle(f"Wind Farm HMI  -  {ENDPOINT}")
main_window.resize(1380, 940)

central = QtWidgets.QWidget()
main_layout = QtWidgets.QVBoxLayout(central)
main_layout.setContentsMargins(12, 12, 12, 12)
main_layout.setSpacing(10)

control_panel = QtWidgets.QFrame()
control_panel.setStyleSheet(
    "QFrame {"
    "background-color: #171c24;"
    "border: 1px solid #2c3442;"
    "border-radius: 12px;"
    "}"
)
control_layout = QtWidgets.QHBoxLayout(control_panel)
control_layout.setContentsMargins(14, 10, 14, 10)
control_layout.setSpacing(18)

max_rows = 4
lights_box = QtWidgets.QGridLayout()
lights_box.setSpacing(6)
lights_box.setContentsMargins(0, 0, 0, 0)
leds: dict[str, LedIndicator] = {}

buttons_box = QtWidgets.QHBoxLayout()
buttons_box.setSpacing(16)
mode_buttons: list[CircularModeButton] = []
button_controls: dict[str, OffOnButton] = {}

def add_light_widget(widget: QtWidgets.QWidget, index: int) -> None:
    """Add a widget at the given index, auto-calculating grid position."""
    col = index // max_rows
    row = index % max_rows
    lights_box.addWidget(widget, row, col)

plots_widget = pg.GraphicsLayoutWidget(title="Live Signals")

control_layout.addLayout(lights_box, 2)
control_layout.addStretch(1)
control_layout.addLayout(buttons_box, 3)

main_layout.addWidget(control_panel)
main_layout.addWidget(plots_widget, 1)

main_window.setCentralWidget(central)
main_window.show()

# ---------------------------------------------------------------------------
# State – initialised lazily from the first received message.
# ---------------------------------------------------------------------------
plots: list[pg.PlotItem] = []
curves: list[list[pg.PlotDataItem]] = []
histories: list[list[deque]] = []
window_size: int = 100
initialized: bool = False
mode_labels: list[str] = ["Auto", "Curtailment", "Safe Shutdown"]


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
        p: pg.PlotItem = plots_widget.addPlot(title=name)
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
            plots_widget.nextRow()

    initialized = True


def update_leds(lights_data: list) -> None:
    for entry in lights_data:
        if len(entry) < 3:
            continue
        name, is_on, color = entry
        key = str(name)
        if key not in leds:
            led = LedIndicator(key, str(color))
            leds[key] = led
            add_light_widget(led, len(leds) - 1)
        leds[key].set_state(bool(is_on))


def send_mode_command(mode_index: int) -> None:
    payload = ["set_mode", int(mode_index)]
    try:
        cmd_sock.send(msgpack.packb(payload, use_bin_type=True), zmq.NOBLOCK)
    except zmq.ZMQError:
        # Controller may not be running yet; keep UI responsive.
        pass


def send_button_command(command_name: str, is_on: bool) -> None:
    payload = ["set_button_state", str(command_name), int(bool(is_on))]
    try:
        cmd_sock.send(msgpack.packb(payload, use_bin_type=True), zmq.NOBLOCK)
    except zmq.ZMQError:
        pass


def update_onoff_button(button_data) -> None:
    global button_controls

    if not isinstance(button_data, list) or len(button_data) < 2:
        return

    is_on = bool(button_data[0])
    command_name = str(button_data[1])
    off_text = f"{command_name} Off"
    on_text = f"{command_name} On"
    if len(button_data) >= 4:
        off_text = str(button_data[2])
        on_text = str(button_data[3])
    elif len(button_data) == 3:
        on_text = str(button_data[2])

    if command_name not in button_controls:
        button = OffOnButton(off_text, on_text, checked=is_on)
        button.toggled.connect(
            lambda checked, cmd=command_name: send_button_command(cmd, checked)
        )
        buttons_box.addWidget(button)
        button_controls[command_name] = button
    else:
        button = button_controls[command_name]
        button.off_text = off_text
        button.on_text = on_text
        button.toggled.disconnect()
        button.toggled.connect(
            lambda checked, cmd=command_name: send_button_command(cmd, checked)
        )
        button.set_state(is_on)


def update_mode_buttons(active_mode: int, labels: list[str]) -> None:
    global mode_buttons, mode_labels

    if labels:
        mode_labels = [str(v) for v in labels]

    if not mode_buttons:
        for idx, label in enumerate(mode_labels):
            btn = CircularModeButton(str(label), idx)
            btn.clicked.connect(lambda _checked, m=idx: send_mode_command(m))
            mode_buttons.append(btn)
            buttons_box.addWidget(btn)
        buttons_box.addStretch(1)

    for btn in mode_buttons:
        btn.setChecked(btn.mode_index == active_mode)


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

    lights_data = []
    controls_data = [0, mode_labels]
    button_data_list = []
    if isinstance(msg, list) and len(msg) >= 5:
        tick, ws, signals, lights_data, controls_data = msg[:5]
        if len(msg) > 5:
            # Collect all remaining elements as button data
            for i in range(5, len(msg)):
                button_data_list.append(msg[i])
    else:
        tick, ws, signals = msg

    if not initialized:
        init_layout(signals, ws)

    if isinstance(lights_data, list):
        update_leds(lights_data)

    if isinstance(controls_data, list) and len(controls_data) >= 2:
        try:
            active_mode = int(controls_data[0])
        except (TypeError, ValueError):
            active_mode = 0
        labels = controls_data[1] if isinstance(controls_data[1], list) else mode_labels
        update_mode_buttons(active_mode, labels)

    # Process all button data
    for button_data in button_data_list:
        if button_data is not None:
            update_onoff_button(button_data)

    x_axis = list(range(window_size))

    for i, (name, unit, labels, values) in enumerate(signals):
        if i >= len(curves):
            break
        for j, v in enumerate(values):
            if j >= len(curves[i]):
                break
            histories[i][j].append(float(v))
            curves[i][j].setData(x=x_axis, y=list(histories[i][j]))

    main_window.setWindowTitle(f"Wind Farm HMI  -  tick {tick}  -  {ENDPOINT}")


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
timer = QtCore.QTimer()
timer.timeout.connect(poll_and_update)
timer.start(POLL_INTERVAL_MS)

if __name__ == "__main__":
    QtWidgets.QApplication.instance().exec()
