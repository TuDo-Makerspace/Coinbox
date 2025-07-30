#!/usr/bin/env python3

"""
Copyright (C) 2025 Yunis <schnackus>,
                   Patrick Pedersen <ctx.xda@gmail.com>,
                   TuDo Makerspace

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
details.

You should have received a copy of the GNU General Public License along with
this program. If not, see <https://www.gnu.org/licenses/>.
"""

""" GUI to Configure the Coinbox """

import sys
import os
import requests
import numpy as np
import pyloudnorm as pyln
import importlib.resources as res
import sys, tempfile

from pathlib import Path
from pydub import AudioSegment
from PySide6 import QtSvg
from PySide6.QtCore import Qt, QTimer, Signal, Slot, QSize, QObject, QThread
from PySide6.QtGui import QPixmap, QIcon
from PySide6.QtWidgets import (
    QApplication,
    QWidget,
    QLabel,
    QPushButton,
    QFileDialog,
    QVBoxLayout,
    QHBoxLayout,
    QStackedWidget,
    QMessageBox,
)
from pathlib import Path
from pydub import AudioSegment, silence

COINBOX_IP = "192.168.0.31"  # Static IP of the Coinbox

#################################################################################
# Path resolution
#################################################################################

_pkg_root = Path(__file__).parent


def path(rel: str) -> Path:
    p = _pkg_root / rel
    if p.exists():
        return str(p)
    try:
        res_path = res.files(__package__).joinpath(rel)
        with res.as_file(res_path) as tmp:
            return os.fspath(tmp)
    except (FileNotFoundError, ModuleNotFoundError):
        raise FileNotFoundError(f"resource '{rel}' not found in package")


#################################################################################
# Audio Conversion and Processing
#################################################################################

SILENCE_THRESH = -50  # dBFS.  Higher = less aggressive trimming
CHUNK_MS = 10  # Analysis granularity for silence detect
# TARGET_PEAK_DB = -0.5  # Desired peak after normalisation
MAX_LENGTH_MS = 5_000  # 5 seconds
TARGET_LUFS = -16.0
CUTOFF_HZ = 1000


def strip_silence(seg):
    lead = silence.detect_leading_silence(
        seg, silence_threshold=SILENCE_THRESH, chunk_size=CHUNK_MS
    )
    tail = silence.detect_leading_silence(
        seg.reverse(), silence_threshold=SILENCE_THRESH, chunk_size=CHUNK_MS
    )
    return seg[lead : len(seg) - tail]


def normalize_lufs(seg: AudioSegment, target_lufs: float = TARGET_LUFS) -> AudioSegment:
    # 1. pydub ➜ np.float32 array in range [-1.0, 1.0)
    samples = np.array(seg.get_array_of_samples()).astype(np.float32)
    peak = float(1 << (8 * seg.sample_width - 1))
    samples /= peak

    # 2. measure loudness
    meter = pyln.Meter(seg.frame_rate)  # uses true‑peak by default
    loudness = meter.integrated_loudness(samples)

    # 3. loudness offset
    gain = target_lufs - loudness  # dB to add (±)
    return seg.apply_gain(gain)


def mp3tosample(inp, out):
    audio = AudioSegment.from_file(inp)

    # 1.  Trim silence
    audio = strip_silence(audio)

    # 2.  Trim to 5 s
    if len(audio) > MAX_LENGTH_MS:
        audio = audio[:MAX_LENGTH_MS]

    # # 3.  Peak-normalise to -0.5 dB
    # change = TARGET_PEAK_DB - audio.max_dBFS  # dB to add (may be + or –)
    # audio = audio.apply_gain(change)

    # 3. High-pass filter to remove low rumble
    audio = audio.high_pass_filter(CUTOFF_HZ)

    # 4.  Loudness normalisation to -16 LUFS
    audio = normalize_lufs(audio, TARGET_LUFS)

    # 5.  Format conversion: mono, 16 kHz, 8-bit unsigned PCM
    audio = (
        audio.set_frame_rate(16_000).set_channels(1).set_sample_width(1)
    )  # 1 byte = 8-bit

    # 6.  Export. pydub hands off to ffmpeg; the extra parameter forces pcm_u8
    audio.export(out, format="wav", parameters=["-acodec", "pcm_u8"])

    print(
        f"Wrote {out} ({len(audio)/1000:.2f}s, "
        f"{audio.frame_rate} Hz, {audio.sample_width*8}-bit unsigned)"
    )


#################################################################################
# Coinbox Discovery
#################################################################################


def discover_coinbox() -> bool:
    try:
        response = requests.get(f"http://{COINBOX_IP}/ping", timeout=3)
        if response.status_code == 200:
            return True
    except requests.RequestException:
        pass
    return False


class _PingWorker(QObject):
    done = Signal(bool)

    @Slot()
    def run(self):
        self.done.emit(discover_coinbox())


class SearchScreen(QWidget):
    found = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        title = QLabel("Searching for Coinbox…", alignment=Qt.AlignCenter)
        font = title.font()
        font.setPointSize(20)
        font.setBold(True)
        title.setFont(font)

        subtitle = QLabel(
            "Please unplug and replug the Coinbox, and make sure no coins are being inserted!",
            alignment=Qt.AlignCenter,
        )

        icon = QLabel(alignment=Qt.AlignCenter)
        icon.setPixmap(
            QPixmap(path("icons/magnifier.svg")).scaled(96, 96, Qt.KeepAspectRatio)
        )

        lay = QVBoxLayout(self)
        lay.addWidget(title)
        lay.addWidget(subtitle)
        lay.addStretch(1)
        lay.addWidget(icon)
        lay.addStretch(1)

        self._timer = QTimer(self, interval=500, timeout=self._start_probe)
        self._timer.start()
        self._thread = None
        self._worker = None

    def _start_probe(self):
        if self._thread and self._thread.isRunning():
            return

        self._thread = QThread()
        self._worker = _PingWorker()
        self._worker.moveToThread(self._thread)

        self._thread.started.connect(self._worker.run)
        self._worker.done.connect(self._on_result)
        self._worker.done.connect(lambda *_: self._thread.quit())
        self._worker.done.connect(lambda *_: self._worker.deleteLater())
        self._thread.finished.connect(lambda: setattr(self, "_thread", None))
        self._thread.finished.connect(self._thread.deleteLater)

        self._thread.start()

    @Slot(bool)
    def _on_result(self, coinbox_found: bool):
        if coinbox_found:
            self._timer.stop()
            self.found.emit()


class FoundScreen(QWidget):
    configure = Signal()  # emitted when the user clicks Configure

    def __init__(self, parent=None):
        super().__init__(parent)
        title = QLabel("Coinbox Found!", alignment=Qt.AlignCenter)
        font = title.font()
        font.setPointSize(20)
        font.setBold(True)
        title.setFont(font)

        btn = QPushButton()
        btn.setIconSize(QSize(96, 96))
        btn.setIcon(QPixmap(path("icons/gear.svg")))
        btn.setFixedSize(120, 120)
        btn.clicked.connect(self.configure.emit)

        lbl = QLabel("Enter config mode", alignment=Qt.AlignCenter)

        lay = QVBoxLayout(self)
        lay.addWidget(title)
        lay.addStretch(1)
        lay.addWidget(btn, alignment=Qt.AlignCenter)
        lay.addWidget(lbl)
        lay.addStretch(1)


#################################################################################
# Coinbox Configuration
#################################################################################

exited = False  # Flag to track if we exited config mode


class ConfigScreen(QWidget):
    exit_config = Signal()  # emitted when Exit button is pressed

    def __init__(self, parent=None):
        super().__init__(parent)
        title = QLabel("Upload sample (mp3)", alignment=Qt.AlignCenter)
        font = title.font()
        font.setPointSize(20)
        font.setBold(True)
        title.setFont(font)

        button_specs = [
            ("1", "70% probability"),
            ("2", "20% probability"),
            ("3", "10% probability"),
        ]
        btn_row = QHBoxLayout()
        for label, subtitle in button_specs:
            btn = self._make_upload_button(label, subtitle)
            btn_row.addWidget(btn, alignment=Qt.AlignCenter)

        reset_btn = QPushButton("Factory reset samples")
        reset_btn.setFixedHeight(40)
        reset_btn.clicked.connect(self._factory_reset)

        exit_btn = QPushButton("Exit and restart Coinbox")
        exit_btn.clicked.connect(self._exit_clicked)
        exit_btn.setFixedHeight(50)

        lay = QVBoxLayout(self)
        lay.addWidget(title)
        lay.addStretch(1)
        lay.addLayout(btn_row)
        lay.addStretch(1)
        lay.addWidget(reset_btn, alignment=Qt.AlignCenter)
        lay.addStretch(1)
        lay.addWidget(exit_btn, alignment=Qt.AlignCenter)
        lay.addStretch(1)

    def _make_upload_button(self, label: str, subtitle: str) -> QWidget:
        btn = QPushButton(label)
        btn.setFixedSize(120, 60)
        btn.clicked.connect(lambda *_: self._upload_sample(int(label)))
        lbl = QLabel(subtitle, alignment=Qt.AlignCenter)
        wrapper = QVBoxLayout()
        box = QWidget()
        wrapper.addWidget(btn)
        wrapper.addWidget(lbl)
        box.setLayout(wrapper)
        return box

    def _upload_sample(self, slot: int):
        path, _ = QFileDialog.getOpenFileName(
            self, f"Select MP3 for slot {slot}", str(Path.home()), "MP3 files (*.mp3)"
        )

        if not path:
            return

        # Convert MP3 to trimmed U8 PCM 16 kHz WAV
        out_path = Path(tempfile.gettempdir()) / f"coinbox_slot{slot}.wav"
        try:
            mp3tosample(path, out_path)
        except Exception as e:
            QMessageBox.critical(self, "Error", f"Failed to convert MP3: {e}")
            return

        # Upload the sample to the Coinbox
        try:
            with open(out_path, "rb") as f:
                response = requests.post(
                    f"http://{COINBOX_IP}/{slot-1}",
                    files={"file": (f"slot{slot}.wav", f, "audio/wav")},
                    timeout=5,
                )
            if response.status_code == 200:
                QMessageBox.information(
                    self, "Success", f"Sample for slot {slot} uploaded!"
                )
            else:
                QMessageBox.critical(
                    self, "Upload Failed", f"Server returned: {response.text}"
                )
        except requests.RequestException as err:
            QMessageBox.critical(
                self, "Network Error", f"Failed to upload sample: {err}"
            )
            return

    def _factory_reset(self):
        reply = QMessageBox.question(
            self,
            "Factory reset",
            "This will erase all three custom samples and restore the "
            "default sounds. Continue?",
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No,
        )
        if reply != QMessageBox.Yes:
            return

        try:
            r = requests.get(f"http://{COINBOX_IP}/reset", timeout=5)
            if r.status_code == 200:
                QMessageBox.information(
                    self, "Reset complete", "Samples were reset to factory defaults."
                )
            else:
                QMessageBox.critical(
                    self,
                    "Reset failed",
                    f"Server replied with status {r.status_code}:\n{r.text}",
                )
        except requests.RequestException as err:
            QMessageBox.critical(self, "Network error", str(err))

    def _exit_clicked(self):
        global exited
        exited = True  # Set the flag to indicate we exited config mode
        try:
            requests.get(f"http://{COINBOX_IP}/restart", timeout=3)
        except requests.RequestException as err:
            QMessageBox.critical(
                self,
                "Failed to exit config mode",
                "Please manually restart the Coinbox.",
            )
            return
        self.exit_config.emit()


#################################################################################
# Main Window
#################################################################################


class MainWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Coinbox")
        self.setWindowIcon(QIcon(path("icons/gear.svg")))
        self.setFixedSize(560, 415)  # matches your mock-up resolution

        self.stack = QStackedWidget(self)
        self.search = SearchScreen()
        self.found = FoundScreen()
        self.config = ConfigScreen()

        self.stack.addWidget(self.search)  # index 0
        self.stack.addWidget(self.found)  # index 1
        self.stack.addWidget(self.config)  # index 2

        lay = QVBoxLayout(self)
        lay.addWidget(self.stack)

        # QApplication.instance().aboutToQuit.connect(_restart_coinbox)

        # wiring
        self.search.found.connect(lambda: self.stack.setCurrentIndex(1))
        self.found.configure.connect(self._configure_coinbox)
        self.config.exit_config.connect(lambda: self.close())

    @Slot()
    def _configure_coinbox(self):
        """Switch to configuration mode."""
        try:
            requests.get(f"http://{COINBOX_IP}/config")
        except requests.RequestException as err:
            QMessageBox.critical(self, "Failed to put Coinbox in config mode", str(err))
            return
        self.stack.setCurrentIndex(2)

    def closeEvent(self, event):
        global exited
        if self.stack.currentIndex() == 2 and not exited:
            # If in config mode, try to restart Coinbox
            try:
                requests.get(f"http://{COINBOX_IP}/restart", timeout=3)
            except requests.RequestException:
                QMessageBox.critical(
                    self,
                    "Failed to exit config mode",
                    "Please manually restart the Coinbox.",
                )
                event.ignore()
                return


def main() -> None:
    app = QApplication(sys.argv)
    win = MainWindow()
    win.setFixedSize(600, 350)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
