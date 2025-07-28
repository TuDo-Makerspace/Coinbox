#!/usr/bin/env python3
from pathlib import Path
import sys
import threading
import requests
from PySide6.QtCore    import Qt, QTimer, Signal, Slot, QSize
from PySide6.QtGui     import QPixmap, QFont
from PySide6.QtWidgets import (
    QApplication, QWidget, QLabel, QPushButton, QFileDialog,
    QVBoxLayout, QHBoxLayout, QStackedWidget, QMessageBox
)

def discover_coinbox() -> bool:
    """Return True once the Coinbox is reachable on the network/USB."""
    # TODO: implement real discovery (mDNS, UDP probe, serial …)
    return True

def http_get(endpoint: str) -> None:
    """Thin wrapper around requests.get with error handling."""
    try:
        requests.get(endpoint, timeout=3)
    except requests.RequestException as exc:
        raise RuntimeError(str(exc)) from exc

class SearchScreen(QWidget):
    found = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        title   = QLabel("Searching for Coinbox…", alignment=Qt.AlignCenter)
        font = title.font()
        font.setPointSize(20)
        font.setBold(True)
        title.setFont(font)

        subtitle= QLabel("Please unplug and replug the Coinbox if it is not found.", alignment=Qt.AlignCenter)

        icon    = QLabel(alignment=Qt.AlignCenter)
        icon.setPixmap(QPixmap("icons/magnifier.svg").scaled(96, 96, Qt.KeepAspectRatio))

        lay = QVBoxLayout(self)
        lay.addWidget(title)
        lay.addWidget(subtitle)
        lay.addStretch(1)
        lay.addWidget(icon)
        lay.addStretch(1)

        self._timer = QTimer(self, interval=500, timeout=self._probe)
        self._timer.start()

    def _probe(self):
        if discover_coinbox():
            self._timer.stop()
            self.found.emit()

class FoundScreen(QWidget):
    configure = Signal()      # emitted when the user clicks Configure

    def __init__(self, parent=None):
        super().__init__(parent)
        title = QLabel("Coinbox Found!", alignment=Qt.AlignCenter)
        font = title.font()
        font.setPointSize(20)
        font.setBold(True)
        title.setFont(font)

        btn   = QPushButton()
        btn.setIconSize(QSize(96, 96))
        btn.setIcon(QPixmap("icons/gear.svg"))
        btn.setFixedSize(120, 120)
        btn.clicked.connect(self.configure.emit)

        lbl   = QLabel("Enter config mode", alignment=Qt.AlignCenter)

        lay = QVBoxLayout(self)
        lay.addWidget(title)
        lay.addStretch(1)
        lay.addWidget(btn, alignment=Qt.AlignCenter)
        lay.addWidget(lbl)
        lay.addStretch(1)

class ConfigScreen(QWidget):
    exit_config = Signal()    # emitted when Exit button is pressed

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
            btn_row.addWidget(btn, 1)

        exit_btn = QPushButton("Exit and restart")
        exit_btn.clicked.connect(self._exit_clicked)
        exit_btn.setFixedHeight(50)

        lay = QVBoxLayout(self)
        lay.addWidget(title)
        lay.addStretch(1)
        lay.addLayout(btn_row)
        lay.addStretch(2)
        lay.addWidget(exit_btn, alignment=Qt.AlignCenter)
        lay.addStretch(1)

    def _make_upload_button(self, label: str, subtitle: str) -> QWidget:
        btn      = QPushButton(label)
        btn.setFixedSize(120, 60)
        btn.clicked.connect(lambda *_: self._upload_sample(int(label)))
        lbl      = QLabel(subtitle, alignment=Qt.AlignCenter)
        wrapper  = QVBoxLayout()
        box      = QWidget()
        wrapper.addWidget(btn)
        wrapper.addWidget(lbl)
        box.setLayout(wrapper)
        return box

    def _upload_sample(self, slot: int):
        path, _ = QFileDialog.getOpenFileName(
            self,
            f"Select MP3 for slot {slot}",
            str(Path.home()),
            "MP3 files (*.mp3)"
        )
        if not path:
            return
        try:
            http_get(f"http://coinbox.local/upload?slot={slot}")  # placeholder
            # TODO: POST the file or stream it, whatever your API needs
            QMessageBox.information(self, "Upload complete", f"Slot {slot} updated.")
        except RuntimeError as err:
            QMessageBox.critical(self, "Upload failed", str(err))

    def _exit_clicked(self):
        try:
            http_get("http://coinbox.local/restart")  # placeholder
        except RuntimeError as err:
            QMessageBox.warning(self, "Warning", f"Restart request failed:\n{err}")
        self.exit_config.emit()

class MainWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Coinbox Uploader")
        self.setFixedSize(560, 415)  # matches your mock-up resolution

        self.stack = QStackedWidget(self)
        self.search  = SearchScreen()
        self.found   = FoundScreen()
        self.config  = ConfigScreen()

        self.stack.addWidget(self.search)   # index 0
        self.stack.addWidget(self.found)    # index 1
        self.stack.addWidget(self.config)   # index 2

        lay = QVBoxLayout(self)
        lay.addWidget(self.stack)

        # wiring
        self.search.found.connect(lambda: self.stack.setCurrentIndex(1))
        self.found.configure.connect(self._configure_coinbox)
        self.config.exit_config.connect(lambda: self.stack.setCurrentIndex(0))

    @Slot()
    def _configure_coinbox(self):
        """Switch to configuration mode."""
        self.stack.setCurrentIndex(2)

def main() -> None:
    app  = QApplication(sys.argv)
    win  = MainWindow()
    win.setFixedSize(450, 250)
    win.show()
    sys.exit(app.exec())

if __name__ == "__main__":
    main()
