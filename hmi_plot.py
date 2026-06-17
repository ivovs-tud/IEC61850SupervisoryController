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
import math
import queue
import threading
import time

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
    IP = sys.argv[1] 
    ENDPOINT = f"tcp://{IP}:5555"
    COMMAND_ENDPOINT = f"tcp://{IP}:5556"
else:
    if sys.platform == "win32":  # covers both 32 and 64-bit Windows
        ENDPOINT = "tcp://172.19.3.214:5555"
        COMMAND_ENDPOINT = "tcp://172.19.3.214:5556"
    else:
        ENDPOINT = "ipc:///tmp/supervisory_controller_hmi.sock"
        COMMAND_ENDPOINT = sys.argv[2] if len(sys.argv) > 2 else "ipc:///tmp/supervisory_controller_hmi_cmd.sock"
#"ipc:///tmp/supervisory_controller_hmi.sock"

POLL_INTERVAL_MS = 50  # how often the Qt timer checks for new ZMQ messages
DEFAULT_SAMPLE_PERIOD_MS = 500
LAYOUT_ROWS = 3
LAYOUT_COLS = 3
RESERVED_GRID_CELLS = 2
MAX_PLOTS = LAYOUT_ROWS * LAYOUT_COLS - RESERVED_GRID_CELLS
TURBINE_MAP_BOUNDS: tuple[float, float, float, float] | None = (0.0, 1900.0, 0.0, 1900.0)
TURBINE_MAP_MARGIN = 600.0
LOCAL_WIND_VECTOR_SCALE = 38.0
GLOBAL_WIND_VECTOR_SCALE = 58.0
GLOBAL_WIND_VECTOR_START_XY = (-700.0, 954.0)
GLOBAL_WIND_LABEL_OFFSET_XY = (0.0, 0.0)
TURBINE_WAKED_SPEED_RATIO_THRESHOLD = 0.95
TURBINE_MIN_WAKE_BRIGHTNESS = 0.35
TURBINE_BODY_LENGTH = 250.0
TURBINE_NACELLE_RADIUS = 22.0
TURBINE_ORIENTATION_OFFSET_DEG = 0.0
TURBINE_ORIENTATION_SIGN = 1.0
INACTIVE_CURVE_ALPHA = 72
ACTIVE_CURVE_ALPHA = 230
SELECTED_CURVE_ALPHA = 255
RATED_ROTOR_SPEED_RPM = 14.0

# Wind-farm map coordinates, ordered as T1, T2, T3, ...
# Leave empty to auto-generate a compact grid from the turbine labels.
TURBINE_COORDINATES_XY: list[tuple[float, float]] = [
    (382.0, 1527.0),   # T1
    (954.0, 1527.0),   # T2
    (1527.0, 1527.0),  # T3
    (382.0, 954.0),    # T4
    (954.0, 954.0),    # T5
    (1527.0, 954.0),   # T6
    (382.0, 382.0),    # T7
    (954.0, 382.0),    # T8
    (1527.0, 382.0),   # T9
]

# Fixed identity colors used everywhere turbine identity is shown.
TURBINE_COLORS: dict[str, tuple[int, int, int]] = {
    "T1": (21, 99, 160),
    "T2": (255, 127, 14),
    "T3": (44, 160, 44),
    "T4": (214, 39, 40),
    "T5": (148, 103, 189),
    "T6": (140, 86, 75),
    "T7": (227, 119, 194),
    "T8": (127, 127, 127),
    "T9": (188, 189, 34),
}

WIND_GLOBAL_COLORS = {
    "wind speed": (34, 213, 238),
    "wind direction": (34, 213, 238),
}

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
pg.setConfigOptions(antialias=True, foreground="w", background="#1b222c")
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


def _qt_brush_enum(name: str):
    brush_style_enum = getattr(QtCore.Qt, "BrushStyle", None)
    if brush_style_enum is not None and hasattr(brush_style_enum, name):
        return getattr(brush_style_enum, name)
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

        def set_color(self, color: tuple[int, int, int]) -> None:
            self._color = color
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

    def set_color(self, color_name: str) -> None:
        self._color = self._color_map.get(color_name.lower(), (230, 54, 54))
        self._face.set_color(self._color)


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


class MiniTurbineButton(QtWidgets.QPushButton):
    def __init__(self, turbine_id: int, checked: bool = True):
        super().__init__(f"T{turbine_id}")
        self.turbine_id = int(turbine_id)
        self.setCheckable(True)
        self.setChecked(bool(checked))
        self.setFixedSize(46, 34)
        self.setToolTip(f"Enable/disable turbine {turbine_id}")
        self.setStyleSheet(
            "QPushButton {"
            "border-radius: 7px;"
            "border: 2px solid #2a3340;"
            "background-color: #232a35;"
            "color: #f0f3f8;"
            "font-size: 13px;"
            "font-weight: 800;"
            "}"
            "QPushButton:checked {"
            "background-color: #1f9d62;"
            "border: 2px solid #8ee3b2;"
            "}"
            "QPushButton:!checked {"
            "background-color: #7a2635;"
            "border: 2px solid #d86b7b;"
            "color: #f7c7ce;"
            "}"
            "QPushButton:hover {"
            "border: 2px solid #f0f3f8;"
            "}"
        )

    def set_state(self, is_on: bool) -> None:
        self.blockSignals(True)
        self.setChecked(bool(is_on))
        self.blockSignals(False)


class AttackResourceWidget(QtWidgets.QFrame):
    def __init__(self):
        super().__init__()
        self.setObjectName("attackResourceWidget")
        self.setStyleSheet(
            "QFrame#attackResourceWidget {"
            "background-color: #151b24;"
            "border: 1px solid #2a3340;"
            "border-radius: 8px;"
            "}"
        )
        self.tap_label = QtWidgets.QLabel("Tap 0/0")
        self.fdi_label = QtWidgets.QLabel("FDI 0/0")
        self.signals_label = QtWidgets.QLabel("FDI signals: none")

        for label in (self.tap_label, self.fdi_label):
            label.setStyleSheet("color: #f0f3f8; font-size: 13px; font-weight: 800;")
        self.signals_label.setStyleSheet("color: #aeb8c7; font-size: 12px; font-weight: 600;")
        self.signals_label.setWordWrap(True)
        self.signals_label.setMaximumWidth(210)

        layout = QtWidgets.QVBoxLayout()
        layout.setContentsMargins(10, 8, 10, 8)
        layout.setSpacing(4)
        row = QtWidgets.QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(12)
        row.addWidget(self.tap_label)
        row.addWidget(self.fdi_label)
        layout.addLayout(row)
        layout.addWidget(self.signals_label)
        self.setLayout(layout)

    def update_usage(self, tap_used: int, tap_total: int, fdi_used: int, fdi_total: int, signals: list[str]) -> None:
        self.tap_label.setText(f"Tap {tap_used}/{tap_total}")
        self.fdi_label.setText(f"FDI {fdi_used}/{fdi_total}")
        signal_text = ", ".join(str(signal) for signal in signals) if signals else "none"
        self.signals_label.setText(f"FDI signals: {signal_text}")


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
    "background-color: #202834;"
    "border: 1px solid #394454;"
    "border-radius: 12px;"
    "}"
)
control_layout = QtWidgets.QHBoxLayout(control_panel)
control_layout.setContentsMargins(14, 10, 14, 10)
control_layout.setSpacing(18)

max_rows = 3
lights_box = QtWidgets.QGridLayout()
lights_box.setSpacing(5)
lights_box.setContentsMargins(0, 0, 0, 0)
leds: dict[str, LedIndicator] = {}

buttons_box = QtWidgets.QHBoxLayout()
buttons_box.setSpacing(16)
mode_buttons: list[CircularModeButton] = []
button_controls: dict[str, OffOnButton] = {}
turbine_enable_buttons: dict[int, MiniTurbineButton] = {}
turbine_enable_box: QtWidgets.QGridLayout | None = None
attack_resource_widget: AttackResourceWidget | None = None
cli_command_queue: queue.Queue[int] = queue.Queue()


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
curve_turbines: list[list[str | None]] = []
curve_signal_names: list[str] = []
curve_labels: list[list[str]] = []
histories: list[list[deque]] = []
no_data_items: list[pg.TextItem] = []
window_size: int = 500
sample_period_ms: int = DEFAULT_SAMPLE_PERIOD_MS
initialized: bool = False
mode_labels: list[str] = ["Auto", "Curtailment", "Safe Shutdown"]
farm_map: pg.PlotItem | None = None
legend_plot: pg.PlotItem | None = None
legend_entries: dict[str, "TurbineLegendEntry"] = {}
active_turbines: set[str] = set()
selected_turbine: str | None = None
map_turbine_glyphs: list[pg.PlotDataItem] = []
map_turbine_labels: list[pg.TextItem] = []
map_local_vectors: list[pg.PlotDataItem] = []
map_local_heads: list[pg.ArrowItem] = []
map_select_points: pg.ScatterPlotItem | None = None
map_global_vector: pg.PlotDataItem | None = None
map_global_head: pg.ArrowItem | None = None
map_global_label: pg.TextItem | None = None
map_layout: list[dict[str, float | str]] = []


class TurbineLegendEntry(pg.GraphicsObject):
    def __init__(self, label: str, color: tuple[int, int, int], select_callback):
        super().__init__()
        self.label = label
        self.color = color
        self.active = True
        self.selected = False
        self._select_callback = select_callback
        self.setAcceptedMouseButtons(QtCore.Qt.MouseButton.LeftButton if hasattr(QtCore.Qt, "MouseButton") else QtCore.Qt.LeftButton)
        self._label_item = pg.TextItem(anchor=(0, 0.5))
        self._label_item.setParentItem(self)
        self._label_item.setPos(22, 12)
        self._update_label()

    def boundingRect(self):
        return QtCore.QRectF(0, 0, 180, 24)

    def paint(self, painter, _option, _widget):
        painter.setRenderHint(_qt_painter_hint("Antialiasing"), True)
        alpha = 235 if self.active else 75
        patch_color = QtGui.QColor(*self.color, alpha)

        painter.setPen(QtGui.QPen(QtGui.QColor(20, 24, 30, alpha), 1))
        painter.setBrush(QtGui.QBrush(patch_color))
        painter.drawRect(QtCore.QRectF(0, 5, 14, 14))

        if self.selected:
            painter.setPen(QtGui.QPen(QtGui.QColor("#f0f3f8"), 2))
            painter.setBrush(QtGui.QBrush(_qt_brush_enum("NoBrush")))
            painter.drawRect(QtCore.QRectF(0, 5, 14, 14))

        if not self.active:
            painter.setPen(QtGui.QPen(QtGui.QColor(238, 243, 249, 120), 1.5))
            painter.drawLine(QtCore.QPointF(22, 12), QtCore.QPointF(65, 12))

    def _update_label(self) -> None:
        color = "#ffffff" if self.selected else ("#eef3f9" if self.active else "#78808a")
        text = f"<s>{self.label}</s>" if not self.active else self.label
        self._label_item.setHtml(
            f"<span style='color: {color}; font-size: 10pt; font-weight: 700;'>{text}</span>"
        )

    def set_active(self, active: bool) -> None:
        self.active = bool(active)
        self._update_label()
        self.update()

    def set_selected(self, selected: bool) -> None:
        self.selected = bool(selected)
        self._update_label()
        self.update()

    def mouseClickEvent(self, event) -> None:
        modifiers = QtWidgets.QApplication.keyboardModifiers()
        control_modifier = QtCore.Qt.KeyboardModifier.ControlModifier if hasattr(QtCore.Qt, "KeyboardModifier") else QtCore.Qt.ControlModifier
        if modifiers & control_modifier:
            toggle_turbine_visibility(self.label)
        else:
            self._select_callback(self.label)
        event.accept()


def _qt_pen_style(name: str):
    """Return Qt pen style value for both Qt5 and Qt6 enum layouts."""
    # Qt6: QtCore.Qt.PenStyle.SolidLine / DashLine
    pen_style_enum = getattr(QtCore.Qt, "PenStyle", None)
    if pen_style_enum is not None and hasattr(pen_style_enum, name):
        return getattr(pen_style_enum, name)
    # Qt5: QtCore.Qt.SolidLine / DashLine
    return getattr(QtCore.Qt, name)


def _scale_rgb(color: tuple[int, int, int], amount: float) -> tuple[int, int, int]:
    return tuple(int(round(channel * amount)) for channel in color)


def _clamp(value: float, minimum: float, maximum: float) -> float:
    return max(minimum, min(maximum, value))


def _is_wind_signal(name: str) -> bool:
    signal_name = str(name).lower()
    return signal_name in ("wind speed", "wind direction")


def _is_global_label(label: str) -> bool:
    return str(label).strip().lower().startswith("global")


def _turbine_id_from_label(label: str) -> str | None:
    first_token = str(label).strip().split(maxsplit=1)[0]
    if first_token in TURBINE_COLORS:
        return first_token
    return None


def _turbine_color(turbine_id: str | None, fallback_index: int = 0) -> tuple[int, int, int]:
    if turbine_id in TURBINE_COLORS:
        return TURBINE_COLORS[turbine_id]
    palette = list(TURBINE_COLORS.values())
    return palette[fallback_index % len(palette)]


def _is_setpoint_label(label: str) -> bool:
    return "setpoint" in str(label).lower()


def _pen_for_curve(signal_name: str, label: str, line_index: int, alpha: int = ACTIVE_CURVE_ALPHA, selected: bool = False):
    turbine_id = _turbine_id_from_label(label)
    color = _turbine_color(turbine_id, line_index)
    line_style = _qt_pen_style("SolidLine")
    width = 3 if selected else 2

    signal_key = str(signal_name).lower()
    if _is_wind_signal(signal_name):
        if _is_global_label(label):
            color = WIND_GLOBAL_COLORS.get(signal_key, (255, 255, 255))
            width = 3
            alpha = ACTIVE_CURVE_ALPHA
        else:
            line_style = _qt_pen_style("DashLine")
    elif _is_setpoint_label(label):
        line_style = _qt_pen_style("DashLine")

    qcolor = QtGui.QColor(*color, alpha)
    return pg.mkPen(color=qcolor, width=width, style=line_style)


def _signal_has_multiple_line_styles(labels: list[str]) -> bool:
    has_measurement = any(not _is_setpoint_label(label) for label in labels)
    has_setpoint = any(_is_setpoint_label(label) for label in labels)
    return has_measurement and has_setpoint


def _legend_pen(color: tuple[int, int, int], style_name: str = "SolidLine", width: int = 2):
    return pg.mkPen(color=color, width=width, style=_qt_pen_style(style_name))


def _add_legend_sample(plot: pg.PlotItem, name: str, pen) -> None:
    plot.plot([], [], pen=pen, name=name)


def _add_plot_legend(plot: pg.PlotItem, signal_name: str, labels: list[str]) -> None:
    signal_key = str(signal_name).strip().lower()
    has_legend = (
        _signal_has_multiple_line_styles(labels)
        or signal_key == "farm reference vs. total power"
        or _is_wind_signal(signal_name)
    )
    if not has_legend:
        return

    legend = plot.addLegend(offset=(10, 10))
    legend.setBrush(pg.mkBrush(0, 0, 0, 160))

    if signal_key == "farm reference vs. total power":
        for idx, label in enumerate(labels):
            _add_legend_sample(plot, str(label), _pen_for_curve(signal_name, str(label), idx))
        return

    if _is_wind_signal(signal_name):
        global_color = WIND_GLOBAL_COLORS.get(signal_key, (255, 255, 255))
        _add_legend_sample(plot, "Global", _legend_pen(global_color, width=3))
        _add_legend_sample(plot, "Turbines", _legend_pen((235, 238, 244), "DashLine", 2))
        return

    _add_legend_sample(plot, "Measurement", _legend_pen((235, 238, 244)))
    _add_legend_sample(plot, "Setpoint", _legend_pen((235, 238, 244), "DashLine"))


def _parse_signal(signal: list):
    name, unit, labels, values = signal[:4]
    y_range = signal[4] if len(signal) >= 5 else None
    return name, unit, labels, values, y_range


def _find_signal(signals: list, wanted_name: str):
    wanted = wanted_name.strip().lower()
    for signal in signals:
        name, unit, labels, values, y_range = _parse_signal(signal)
        if str(name).strip().lower() == wanted:
            return name, unit, labels, values, y_range
    return None


def _to_float(value, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _vector_components(speed, direction_deg) -> tuple[float, float]:
    magnitude = max(max(0.0, _to_float(speed)), 2.0)
    radians = math.radians(_to_float(direction_deg) % 360.0)
    return magnitude * math.sin(radians), magnitude * math.cos(radians)


def _direction_angle_deg(dx: float, dy: float) -> float:
    return math.degrees(math.pi + math.atan2(dy, dx))


def _arrow_item_angle_deg(dx: float, dy: float) -> float:
    # ArrowItem's 0 degrees points upward, while the plotted stem angle is
    # measured counter-clockwise from +x. Convert so head and stem line up.
    return 90.0 - _direction_angle_deg(dx, dy)


def _build_turbine_layout(labels: list[str]) -> list[dict[str, float | str]]:
    turbine_labels = [str(label) for label in labels if not _is_global_label(str(label))]
    if TURBINE_COORDINATES_XY:
        layout: list[dict[str, float | str]] = []
        for idx, label in enumerate(turbine_labels):
            if idx < len(TURBINE_COORDINATES_XY):
                x, y = TURBINE_COORDINATES_XY[idx]
            else:
                x, y = _auto_turbine_coordinate(idx, len(turbine_labels))
            layout.append({"label": label, "x": float(x), "y": float(y)})
        return layout

    return _build_auto_turbine_layout(turbine_labels)


def _auto_turbine_coordinate(idx: int, count: int) -> tuple[float, float]:
    count = max(1, count)
    cols = max(1, math.ceil(math.sqrt(count)))
    rows = max(1, math.ceil(count / cols))
    spacing = 500.0
    row = idx // cols
    col = idx % cols
    x = (col - (cols - 1) / 2.0) * spacing
    y = ((rows - 1) / 2.0 - row) * spacing
    return x, y


def _build_auto_turbine_layout(turbine_labels: list[str]) -> list[dict[str, float | str]]:
    layout: list[dict[str, float | str]] = []

    for idx, label in enumerate(turbine_labels):
        x, y = _auto_turbine_coordinate(idx, len(turbine_labels))
        layout.append({"label": label, "x": x, "y": y})

    return layout


def _values_by_label(labels: list, values: list) -> dict[str, float]:
    result: dict[str, float] = {}
    for label, value in zip(labels, values):
        result[str(label)] = _to_float(value)
    return result


def _scaled_vector(speed, direction_deg, scale: float = 18.0) -> tuple[float, float]:
    dx, dy = _vector_components(speed, direction_deg)
    return dx * scale, dy * scale


def _set_farm_map_bounds(xs: list[float], ys: list[float]) -> None:
    if farm_map is None:
        return

    if TURBINE_MAP_BOUNDS is not None:
        x_min, x_max, y_min, y_max = TURBINE_MAP_BOUNDS
    else:
        x_min = min(xs) - TURBINE_MAP_MARGIN
        x_max = max(xs) + TURBINE_MAP_MARGIN
        y_min = min(ys) - TURBINE_MAP_MARGIN
        y_max = max(ys) + TURBINE_MAP_MARGIN

    farm_map.setXRange(float(x_min), float(x_max), padding=0)
    farm_map.setYRange(float(y_min), float(y_max), padding=0)


def _wake_color(local_speed: float, global_speed: float, base_color: tuple[int, int, int]) -> tuple[int, int, int]:
    if global_speed <= 0.0:
        return base_color

    local_to_global = _clamp(local_speed / global_speed, 0.0, 1.25)
    wake_threshold = max(0.001, TURBINE_WAKED_SPEED_RATIO_THRESHOLD)
    brightness = TURBINE_MIN_WAKE_BRIGHTNESS + (1.0 - TURBINE_MIN_WAKE_BRIGHTNESS) * _clamp(local_to_global / wake_threshold, 0.0, 1.0)
    return _scale_rgb(base_color, brightness)


def _turbine_pen(turbine_id: str, local_speed: float, global_speed: float, width: float = 2.5):
    color = _wake_color(local_speed, global_speed, _turbine_color(turbine_id))
    alpha = SELECTED_CURVE_ALPHA if turbine_id == selected_turbine else (120 if selected_turbine else ACTIVE_CURVE_ALPHA)
    return pg.mkPen(color=QtGui.QColor(*color, alpha), width=width)


def _orientation_values_by_label(signals: list) -> dict[str, float]:
    yaw_signal = _find_signal(signals, "Turbine Orientation and Setpoints")
    if yaw_signal is None:
        return {}

    _name, _unit, labels, values, _y_range = yaw_signal
    yaw_values = _values_by_label(labels, values)
    orientations: dict[str, float] = {}

    for turbine in map_layout:
        label = str(turbine["label"])
        orientations[label] = yaw_values.get(f"{label} Orientation", yaw_values.get(label, 0.0))

    return orientations


def _turbine_shape_points(x: float, y: float, orientation_deg: float) -> tuple[list[float], list[float]]:
    heading_deg = TURBINE_ORIENTATION_OFFSET_DEG + TURBINE_ORIENTATION_SIGN * _to_float(orientation_deg)
    radians = math.radians(heading_deg % 360.0)

    # Compass convention: 0 deg is north/up, 90 deg is east/right.
    heading_x = math.sin(radians)
    heading_y = math.cos(radians)
    right_x = math.cos(radians)
    right_y = -math.sin(radians)

    left_x = x - right_x * TURBINE_BODY_LENGTH * 0.5
    left_y = y - right_y * TURBINE_BODY_LENGTH * 0.5
    right_tip_x = x + right_x * TURBINE_BODY_LENGTH * 0.5
    right_tip_y = y + right_y * TURBINE_BODY_LENGTH * 0.5

    xs = [left_x, right_tip_x, float("nan")]
    ys = [left_y, right_tip_y, float("nan")]

    for i in range(13):
        a = -math.pi / 2.0 + math.pi * i / 12.0
        lateral = math.sin(a) * TURBINE_NACELLE_RADIUS
        rearward = math.cos(a) * TURBINE_NACELLE_RADIUS
        arc_x = x + right_x * lateral - heading_x * rearward
        arc_y = y + right_y * lateral - heading_y * rearward
        xs.append(arc_x)
        ys.append(arc_y)

    return xs, ys


def init_farm_map(signals: list) -> None:
    global farm_map, map_turbine_glyphs, map_turbine_labels, map_local_vectors
    global map_local_heads, map_global_vector, map_global_head, map_global_label, map_layout, map_select_points

    wind_speed_signal = _find_signal(signals, "Wind Speed")
    if wind_speed_signal is not None:
        _name, _unit, labels, _values, _y_range = wind_speed_signal
    else:
        labels = [f"T{i + 1}" for i in range(9)]

    map_layout = _build_turbine_layout(labels)
    farm_map = plots_widget.addPlot(row=LAYOUT_ROWS - 1, col=1, title="Live Turbine Wind Map")
    farm_map.setLabel("left", "Northing")
    farm_map.setLabel("bottom", "Easting")
    farm_map.showGrid(x=True, y=True, alpha=0.25)
    farm_map.setAspectLocked(True)

    xs = [float(t["x"]) for t in map_layout]
    ys = [float(t["y"]) for t in map_layout]

    for turbine in map_layout:
        turbine_id = str(turbine["label"])
        turbine_color = _turbine_color(turbine_id)
        glyph = farm_map.plot([], [], pen=pg.mkPen(color=turbine_color, width=2.5))
        map_turbine_glyphs.append(glyph)

        label = pg.TextItem(str(turbine["label"]), color="#f4f7fb", anchor=(0.5, 1.35))
        label.setPos(float(turbine["x"]), float(turbine["y"]))
        farm_map.addItem(label)
        map_turbine_labels.append(label)

        vector = farm_map.plot([], [], pen=pg.mkPen(color=turbine_color, width=2))
        head = pg.ArrowItem(angle=0, headLen=14, tailLen=0, brush=turbine_color, pen=pg.mkPen(turbine_color))
        farm_map.addItem(head)
        map_local_vectors.append(vector)
        map_local_heads.append(head)

    map_select_points = pg.ScatterPlotItem()
    map_select_points.sigClicked.connect(lambda _plot, points: select_turbine(str(points[0].data())) if points else None)
    farm_map.addItem(map_select_points)

    map_global_vector = farm_map.plot([], [], pen=pg.mkPen(color=WIND_GLOBAL_COLORS["wind speed"], width=4))
    map_global_head = pg.ArrowItem(angle=0, headLen=18, tailLen=0, brush = WIND_GLOBAL_COLORS["wind speed"],pen=pg.mkPen(color=WIND_GLOBAL_COLORS["wind speed"], width=4))
    farm_map.addItem(map_global_head)
    map_global_label = pg.TextItem("Global", color="#ffe66d", anchor=(0.5, -0.2))
    farm_map.addItem(map_global_label)

    _set_farm_map_bounds(xs, ys)


def apply_turbine_visibility() -> None:
    for signal_name, sig_labels, sig_curves, sig_turbines in zip(curve_signal_names, curve_labels, curves, curve_turbines):
        for line_index, (curve, turbine_id, label) in enumerate(zip(sig_curves, sig_turbines, sig_labels)):
            if turbine_id is not None:
                curve.setVisible(turbine_id in active_turbines)
                is_selected = selected_turbine == turbine_id
                alpha = SELECTED_CURVE_ALPHA if is_selected else (INACTIVE_CURVE_ALPHA if selected_turbine else ACTIVE_CURVE_ALPHA)
                curve.setPen(_pen_for_curve(signal_name, label, line_index, alpha=alpha, selected=is_selected))

    for turbine_id, entry in legend_entries.items():
        entry.set_active(turbine_id in active_turbines)
        entry.set_selected(turbine_id == selected_turbine)

    if map_select_points is not None:
        spots = []
        for turbine in map_layout:
            turbine_id = str(turbine["label"])
            color = _turbine_color(turbine_id)
            alpha = 255 if turbine_id == selected_turbine else 80
            spots.append({
                "pos": (float(turbine["x"]), float(turbine["y"])),
                "data": turbine_id,
                "size": 34 if turbine_id == selected_turbine else 26,
                "brush": pg.mkBrush(255, 255, 255, 0),
                "pen": pg.mkPen(QtGui.QColor(*color, alpha), width=3 if turbine_id == selected_turbine else 1),
            })
        map_select_points.setData(spots)


def select_turbine(turbine_id: str) -> None:
    global selected_turbine

    selected_turbine = None if selected_turbine == turbine_id else turbine_id
    apply_turbine_visibility()


def toggle_turbine_visibility(turbine_id: str) -> None:
    if turbine_id in active_turbines:
        active_turbines.remove(turbine_id)
    else:
        active_turbines.add(turbine_id)
    apply_turbine_visibility()


def init_turbine_legend(turbine_labels: list[str]) -> None:
    global legend_plot, active_turbines

    legend_plot = plots_widget.addPlot(row=LAYOUT_ROWS - 1, col=2, title="Turbines")
    legend_plot.hideAxis("left")
    legend_plot.hideAxis("bottom")
    legend_plot.setMouseEnabled(x=False, y=False)
    legend_plot.setMenuEnabled(False)
    legend_plot.setXRange(0, 220, padding=0)
    legend_plot.setYRange(0, max(1, len(turbine_labels)) * 28 + 8, padding=0)

    active_turbines = set(turbine_labels)
    legend_entries.clear()
    for idx, turbine_id in enumerate(turbine_labels):
        entry = TurbineLegendEntry(turbine_id, _turbine_color(turbine_id), select_turbine)
        entry.setPos(12, max(1, len(turbine_labels)) * 28 - idx * 28 - 22)
        legend_plot.addItem(entry)
        legend_entries[turbine_id] = entry


def _valid_y_range(y_range) -> tuple[float, float] | None:
    if not isinstance(y_range, (list, tuple)) or len(y_range) < 2:
        return None
    try:
        y_min = float(y_range[0])
        y_max = float(y_range[1])
    except (TypeError, ValueError):
        return None
    if y_min >= y_max:
        return None
    return y_min, y_max


def _nice_time_step_seconds(total_seconds: float) -> int:
    target = max(1.0, total_seconds / 4.0)
    candidates = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600]
    return min(candidates, key=lambda value: abs(value - target))


def _relative_time_ticks(size: int, period_ms: int) -> list[tuple[float, str]]:
    if size <= 1:
        return [(0.0, "Now")]

    right = float(size - 1)
    period_s = max(0.001, float(period_ms) / 1000.0)
    total_seconds = right * period_s
    step_seconds = _nice_time_step_seconds(total_seconds)
    oldest_tick_seconds = int(total_seconds // step_seconds) * step_seconds

    ticks: list[tuple[float, str]] = []
    for age_seconds in range(oldest_tick_seconds, 0, -step_seconds):
        position = right - (float(age_seconds) / period_s)
        if position >= 0.0:
            ticks.append((position, f"-{age_seconds}sec"))

    ticks.append((right, "Now"))

    return ticks


def _apply_relative_time_axis(plot: pg.PlotItem, size: int, period_ms: int) -> None:
    plot.setLabel("bottom", "")
    plot.getAxis("bottom").setTicks([_relative_time_ticks(size, period_ms)])


class ValueAxisItem(pg.AxisItem):
    def __init__(self, unit: str, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.raw_unit = str(unit)
        self.display_unit = self.raw_unit
        self.scale_factor = 1.0
        if self.raw_unit == "W":
            self.display_unit = "MW"
            self.scale_factor = 1e-6
        elif self.raw_unit == "Nm":
            self.display_unit = "kNm"
            self.scale_factor = 1e-3

    def tickStrings(self, values, scale, spacing):
        labels = []
        for value in values:
            scaled = float(value) * self.scale_factor
            if abs(scaled) >= 100:
                labels.append(f"{scaled:.0f}")
            elif abs(scaled) >= 10:
                labels.append(f"{scaled:.1f}".rstrip("0").rstrip("."))
            else:
                labels.append(f"{scaled:.2f}".rstrip("0").rstrip("."))
        return labels


def _has_signal_data(values: list) -> bool:
    for value in values:
        try:
            numeric = float(value)
        except (TypeError, ValueError):
            continue
        if math.isfinite(numeric) and abs(numeric) > 1e-9:
            return True
    return False


def init_layout(signals: list, ws: int) -> None:
    """Build all PlotItems and PlotDataItems from the first message."""
    global plots, curves, curve_turbines, histories, window_size, initialized

    window_size = ws
    x_axis = list(range(window_size))

    for idx, signal in enumerate(signals[:MAX_PLOTS]):
        name, unit, labels, _values, y_range = _parse_signal(signal)
        left_axis = ValueAxisItem(unit, orientation="left")
        p: pg.PlotItem = plots_widget.addPlot(title=name, axisItems={"left": left_axis})
        p.setLabel("left", left_axis.display_unit)
        _apply_relative_time_axis(p, window_size, sample_period_ms)
        p.showGrid(x=True, y=True, alpha=0.25)
        p.setXRange(0, window_size - 1 + max(2.0, window_size * 0.025), padding=0)
        fixed_y_range = _valid_y_range(y_range)
        if fixed_y_range is not None:
            p.setYRange(fixed_y_range[0], fixed_y_range[1], padding=0)
        _add_plot_legend(p, name, labels)

        if str(name).strip().lower() == "rotor speed":
            p.addLine(
                y=RATED_ROTOR_SPEED_RPM,
                pen=pg.mkPen(color=(245, 248, 252), width=2),
            )
            rated_label = pg.TextItem("Rated", color="#f5f8fc", anchor=(0.5, 1.0))
            rated_label.setPos(window_size * 0.94, RATED_ROTOR_SPEED_RPM + 0.25)
            p.addItem(rated_label)

        sig_curves: list[pg.PlotDataItem] = []
        sig_turbines: list[str | None] = []
        sig_histories: list[deque] = []

        for j, label in enumerate(labels):
            c = p.plot(
                x=x_axis,
                y=[0.0] * window_size,
                pen=_pen_for_curve(name, label, j),
            )
            sig_curves.append(c)
            sig_turbines.append(_turbine_id_from_label(label))
            sig_histories.append(deque([0.0] * window_size, maxlen=window_size))

        no_data_item = pg.TextItem("No data", color="#9aa6b6", anchor=(0.5, 0.5))
        no_data_item.setVisible(False)
        p.addItem(no_data_item)

        plots.append(p)
        curves.append(sig_curves)
        curve_turbines.append(sig_turbines)
        curve_signal_names.append(str(name))
        curve_labels.append([str(label) for label in labels])
        histories.append(sig_histories)
        no_data_items.append(no_data_item)

        if (idx + 1) % LAYOUT_COLS == 0:
            plots_widget.nextRow()

    init_farm_map(signals)
    init_turbine_legend([str(turbine["label"]) for turbine in map_layout])
    apply_turbine_visibility()
    initialized = True


def update_farm_map(signals: list) -> None:
    if farm_map is None or not map_layout:
        return

    wind_speed_signal = _find_signal(signals, "Wind Speed")
    wind_direction_signal = _find_signal(signals, "Wind Direction")
    if wind_speed_signal is None or wind_direction_signal is None:
        return

    _name, _unit, speed_labels, speed_values, _y_range = wind_speed_signal
    _name, _unit, direction_labels, direction_values, _y_range = wind_direction_signal
    speeds = _values_by_label(speed_labels, speed_values)
    directions = _values_by_label(direction_labels, direction_values)

    orientations = _orientation_values_by_label(signals)
    global_speed = speeds.get("Global", 0.0)

    for idx, turbine in enumerate(map_layout):
        label = str(turbine["label"])
        x = float(turbine["x"])
        y = float(turbine["y"])
        local_speed = speeds.get(label, 0.0)
        is_selected = label == selected_turbine
        local_pen = _turbine_pen(label, local_speed, global_speed, width=4.0 if is_selected else 2.5)
        dx, dy = _scaled_vector(local_speed, directions.get(label, 0.0), scale=LOCAL_WIND_VECTOR_SCALE)
        end_x = x - dx
        end_y = y - dy

        if idx < len(map_turbine_glyphs):
            glyph_x, glyph_y = _turbine_shape_points(x, y, orientations.get(label, 0.0))
            map_turbine_glyphs[idx].setData(glyph_x, glyph_y)
            map_turbine_glyphs[idx].setPen(local_pen)
        if idx < len(map_local_vectors):
            map_local_vectors[idx].setData([x, end_x], [y, end_y])
            map_local_vectors[idx].setPen(_turbine_pen(label, local_speed, global_speed, width=3.0 if is_selected else 2.0))
        if idx < len(map_local_heads):
            map_local_heads[idx].setPos(end_x, end_y)
            local_color = _wake_color(local_speed, global_speed, _turbine_color(label))
            local_alpha = 255 if is_selected else (130 if selected_turbine else 230)
            map_local_heads[idx].setStyle(
                angle=90+_arrow_item_angle_deg(dx, dy),
                brush=QtGui.QColor(*local_color, local_alpha),
                pen=pg.mkPen(color=QtGui.QColor(*local_color, local_alpha), width=2.0 if is_selected else 1.5),
            )

    global_direction = directions.get("Global", 0.0)
    dx, dy = _scaled_vector(global_speed, global_direction, scale=GLOBAL_WIND_VECTOR_SCALE)
    start_x, start_y = GLOBAL_WIND_VECTOR_START_XY
    end_x = start_x - dx
    end_y = start_y - dy

    if map_global_vector is not None:
        map_global_vector.setData([start_x, end_x], [start_y, end_y])
    if map_global_head is not None:
        map_global_head.setPos(end_x, end_y)
        map_global_head.setStyle(angle=90+_arrow_item_angle_deg(dx, dy))
    if map_global_label is not None:
        label_dx, label_dy = GLOBAL_WIND_LABEL_OFFSET_XY
        map_global_label.setText(f"Global {global_speed:.1f} m/s, {global_direction:.0f} deg")
        map_global_label.setPos(3000, 954)


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
        leds[key].set_color(str(color))
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


def send_turbine_enable_command(turbine_id: int, is_on: bool) -> None:
    payload = ["set_turbine_enable", int(turbine_id), int(bool(is_on))]
    try:
        cmd_sock.send(msgpack.packb(payload, use_bin_type=True), zmq.NOBLOCK)
    except zmq.ZMQError:
        pass


def read_cli_commands() -> None:
    print("HMI CLI: type a turbine id and press Enter to show/hide that turbine's plots.")
    for line in sys.stdin:
        command = line.strip()
        if not command:
            continue
        try:
            cli_command_queue.put(int(command))
        except ValueError:
            print("HMI CLI: enter a numeric turbine id.")


def drain_cli_commands() -> None:
    if not initialized:
        return

    while True:
        try:
            turbine_id = cli_command_queue.get_nowait()
        except queue.Empty:
            break
        turbine_label = f"T{turbine_id}"
        if turbine_label in legend_entries:
            toggle_turbine_visibility(turbine_label)
        else:
            print(f"HMI CLI: turbine id {turbine_id} is not in this HMI layout.")


def update_turbine_enable_buttons(payload) -> None:
    global turbine_enable_box

    if not isinstance(payload, list) or len(payload) < 2:
        return
    states = payload[1]
    if not isinstance(states, list):
        return

    if turbine_enable_box is None:
        turbine_enable_box = QtWidgets.QGridLayout()
        turbine_enable_box.setContentsMargins(0, 0, 0, 0)
        turbine_enable_box.setSpacing(6)
        buttons_box.addLayout(turbine_enable_box)

    for index, state in enumerate(states):
        turbine_id = index + 1
        is_enabled = bool(state)
        if turbine_id not in turbine_enable_buttons:
            button = MiniTurbineButton(turbine_id, checked=is_enabled)
            button.toggled.connect(
                lambda checked, tid=turbine_id: send_turbine_enable_command(tid, checked)
            )
            turbine_enable_buttons[turbine_id] = button
            turbine_enable_box.addWidget(button, index // 3, index % 3)
        else:
            turbine_enable_buttons[turbine_id].set_state(is_enabled)


def update_no_data_overlay(plot_index: int, values: list) -> None:
    if plot_index >= len(no_data_items) or plot_index >= len(plots):
        return

    item = no_data_items[plot_index]
    has_data = _has_signal_data(values)
    item.setVisible(not has_data)
    if not has_data:
        x_range, y_range = plots[plot_index].viewRange()
        item.setPos((x_range[0] + x_range[1]) * 0.5, (y_range[0] + y_range[1]) * 0.5)


def update_attack_resources(payload) -> None:
    global attack_resource_widget

    if not isinstance(payload, list) or len(payload) < 6:
        return

    try:
        tap_used = int(payload[1])
        tap_total = int(payload[2])
        fdi_used = int(payload[3])
        fdi_total = int(payload[4])
    except (TypeError, ValueError):
        return

    signals = payload[5] if isinstance(payload[5], list) else []
    if attack_resource_widget is None:
        attack_resource_widget = AttackResourceWidget()
        buttons_box.addWidget(attack_resource_widget)
    attack_resource_widget.update_usage(tap_used, tap_total, fdi_used, fdi_total, signals)


def update_sample_period(payload) -> None:
    global sample_period_ms

    if not isinstance(payload, list) or len(payload) < 2:
        return
    try:
        period_ms = int(payload[1])
    except (TypeError, ValueError):
        return
    if period_ms > 0:
        sample_period_ms = period_ms


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
        button.toggled.connect(button._update_label)
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

    drain_cli_commands()

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

    for button_data in button_data_list:
        if isinstance(button_data, list) and button_data and button_data[0] == "sample_period_ms":
            update_sample_period(button_data)

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
            if isinstance(button_data, list) and button_data:
                payload_type = button_data[0]
                if payload_type == "turbine_enable_states":
                    update_turbine_enable_buttons(button_data)
                elif payload_type == "attack_resources":
                    update_attack_resources(button_data)
                elif payload_type == "sample_period_ms":
                    update_sample_period(button_data)
                else:
                    update_onoff_button(button_data)

    x_axis = list(range(window_size))

    for i, signal in enumerate(signals):
        if i >= len(curves):
            break
        _name, _unit, _labels, values, _y_range = _parse_signal(signal)
        update_no_data_overlay(i, values)
        for j, v in enumerate(values):
            if j >= len(curves[i]):
                break
            histories[i][j].append(float(v))
            curves[i][j].setData(x=x_axis, y=list(histories[i][j]))

    update_farm_map(signals)
    main_window.setWindowTitle(f"Wind Farm HMI  -  tick {tick}  -  {ENDPOINT}")


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
cli_thread = threading.Thread(target=read_cli_commands, daemon=True)
cli_thread.start()

timer = QtCore.QTimer()
timer.timeout.connect(poll_and_update)
timer.start(POLL_INTERVAL_MS)

if __name__ == "__main__":
    QtWidgets.QApplication.instance().exec()
