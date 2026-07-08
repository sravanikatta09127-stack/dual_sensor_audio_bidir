
#!/usr/bin/env python3
"""
Hailo Dual Sensor Receiver
--------------------------
Production host-side app for dual_sensor_audio_bidir.

Receives:
  :5000  sensor 0 video H264
  :5100  sensor 1 video H264
  :5200  board audio L16 stereo

Sends:
  :5201  host mic audio L16 stereo to board

Usage:
  python3 host_receiver.py
  python3 host_receiver.py --board-ip 10.0.0.1
  python3 host_receiver.py --board-ip 10.0.0.1 --no-mic
"""

import sys
import argparse
import signal

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstVideo", "1.0")
gi.require_version("Gtk", "3.0")
from gi.repository import Gst, GstVideo, Gtk, GLib, Gdk

# ─── constants ────────────────────────────────────────────────
PORT_VIDEO_S0  = 5000
PORT_VIDEO_S1  = 5100
PORT_AUDIO_IN  = 5200
PORT_AUDIO_OUT = 5201
JITTER_MS      = 200

VIDEO_CAPS = (
    "application/x-rtp,media=video,clock-rate=90000,"
    "encoding-name=H264,payload=96"
)
AUDIO_CAPS_IN = (
    "application/x-rtp,media=audio,clock-rate=48000,"
    "encoding-name=L16,channels=2,payload=96"
)


# ─── pipeline builders ────────────────────────────────────────
def build_video_pipeline(port):
    desc = (
        f'udpsrc address=0.0.0.0 port={port} caps="{VIDEO_CAPS}" '
        f'! rtpjitterbuffer latency={JITTER_MS} '
        f'! rtph264depay ! avdec_h264 ! videoconvert '
        f'! gtksink name=vsink sync=false'
    )
    pipe = Gst.parse_launch(desc)
    sink = pipe.get_by_name("vsink")
    return pipe, sink


def build_audio_in_pipeline():
    desc = (
        f'udpsrc address=0.0.0.0 port={PORT_AUDIO_IN} caps="{AUDIO_CAPS_IN}" '
        f'! rtpjitterbuffer latency={JITTER_MS} '
        f'! rtpL16depay ! audioconvert ! audioresample '
        f'! autoaudiosink sync=false'
    )
    return Gst.parse_launch(desc)


def build_audio_out_pipeline(board_ip):
    desc = (
        f'alsasrc device=default '
        f'! audioconvert ! audioresample '
        f'! audio/x-raw,rate=48000,channels=2,format=S16LE '
        f'! audioconvert '
        f'! audio/x-raw,rate=48000,channels=2,format=S16BE '
        f'! rtpL16pay pt=96 '
        f'! udpsink host={board_ip} port={PORT_AUDIO_OUT} sync=false'
    )
    return Gst.parse_launch(desc)


# ─── CSS styling ──────────────────────────────────────────────
CSS = b"""
window {
    background-color: #111111;
}
.title-bar {
    background-color: #1a1a1a;
    padding: 10px 16px;
    border-bottom: 1px solid #2a2a2a;
}
.title-label {
    color: #ffffff;
    font-size: 15px;
    font-weight: bold;
}
.board-label {
    color: #666666;
    font-size: 12px;
}
.status-live {
    color: #22c55e;
    font-size: 12px;
    font-weight: bold;
}
.status-connecting {
    color: #f59e0b;
    font-size: 12px;
}
.status-error {
    color: #ef4444;
    font-size: 12px;
}
.channel-label {
    color: #aaaaaa;
    font-size: 11px;
    padding: 4px 0;
    background-color: #1a1a1a;
}
.video-frame {
    background-color: #000000;
    border: 1px solid #2a2a2a;
}
.bottom-bar {
    background-color: #1a1a1a;
    padding: 8px 16px;
    border-top: 1px solid #2a2a2a;
}
.info-label {
    color: #555555;
    font-size: 11px;
}
.mic-btn {
    background-color: #1d4ed8;
    color: #ffffff;
    border: none;
    border-radius: 4px;
    padding: 4px 12px;
    font-size: 12px;
}
.mic-btn:checked {
    background-color: #15803d;
}
.quit-btn {
    background-color: #1f2937;
    color: #cccccc;
    border: 1px solid #374151;
    border-radius: 4px;
    padding: 4px 12px;
    font-size: 12px;
}
.quit-btn:hover {
    background-color: #374151;
}
"""


# ─── main application class ───────────────────────────────────
class ReceiverApp:

    def __init__(self, board_ip, enable_mic):
        self.board_ip      = board_ip
        self.enable_mic    = enable_mic
        self.pipe_v0       = None
        self.pipe_v1       = None
        self.pipe_audio_in = None
        self.pipe_audio_out= None
        self.all_pipes     = []
        self._build_ui()

    # ── UI ────────────────────────────────────────────────────
    def _build_ui(self):
        # apply CSS
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_screen(
            Gdk.Screen.get_default(),
            provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
        )

        self.win = Gtk.Window(title="Hailo Dual Sensor Receiver")
        self.win.set_default_size(1280, 580)
        self.win.connect("delete-event", self._on_quit)

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.win.add(root)

        # title bar
        tbar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        tbar.get_style_context().add_class("title-bar")

        tlbl = Gtk.Label(label="Hailo-15  Live View")
        tlbl.get_style_context().add_class("title-label")
        tlbl.set_halign(Gtk.Align.START)
        tbar.pack_start(tlbl, False, False, 0)

        blbl = Gtk.Label(label=f"board {self.board_ip}")
        blbl.get_style_context().add_class("board-label")
        blbl.set_halign(Gtk.Align.START)
        tbar.pack_start(blbl, True, True, 0)

        self.status_lbl = Gtk.Label(label="● connecting")
        self.status_lbl.get_style_context().add_class("status-connecting")
        tbar.pack_end(self.status_lbl, False, False, 0)

        root.pack_start(tbar, False, False, 0)

        # video area
        video_box = Gtk.Box(
            orientation=Gtk.Orientation.HORIZONTAL, spacing=2
        )
        video_box.set_margin_top(2)
        video_box.set_margin_bottom(2)
        video_box.set_margin_start(2)
        video_box.set_margin_end(2)

        self.slot0 = self._make_slot("Sensor 0  ·  video + audio")
        self.slot1 = self._make_slot("Sensor 1  ·  video only")
        video_box.pack_start(self.slot0["frame"], True, True, 0)
        video_box.pack_start(self.slot1["frame"], True, True, 0)
        root.pack_start(video_box, True, True, 0)

        # bottom bar
        bbar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        bbar.get_style_context().add_class("bottom-bar")

        info = Gtk.Label(
            label=f"video :5000 · :5100   audio in :5200   audio out :5201"
        )
        info.get_style_context().add_class("info-label")
        info.set_halign(Gtk.Align.START)
        bbar.pack_start(info, True, True, 0)

        self.mic_btn = Gtk.ToggleButton(
            label="● Mic ON" if self.enable_mic else "○ Mic OFF"
        )
        self.mic_btn.set_active(self.enable_mic)
        self.mic_btn.get_style_context().add_class("mic-btn")
        self.mic_btn.connect("toggled", self._on_mic_toggled)
        bbar.pack_end(self.mic_btn, False, False, 0)

        qbtn = Gtk.Button(label="Stop & Quit")
        qbtn.get_style_context().add_class("quit-btn")
        qbtn.connect("clicked", self._on_quit)
        bbar.pack_end(qbtn, False, False, 0)

        root.pack_start(bbar, False, False, 0)

        self.win.show_all()

    def _make_slot(self, label_text):
        frame = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        frame.get_style_context().add_class("video-frame")

        lbl = Gtk.Label(label=label_text)
        lbl.get_style_context().add_class("channel-label")
        frame.pack_start(lbl, False, False, 0)

        # placeholder until gtksink widget is embedded
        placeholder = Gtk.DrawingArea()
        placeholder.set_size_request(630, 360)
        frame.pack_start(placeholder, True, True, 0)

        return {"frame": frame, "placeholder": placeholder, "label": lbl}

    # ── pipeline startup ──────────────────────────────────────
    def start_pipelines(self):
        print("[INFO] Starting pipelines…")

        # video 0
        self.pipe_v0, vsink0 = build_video_pipeline(PORT_VIDEO_S0)
        self._inject_gtksink(vsink0, self.slot0)
        self._attach_bus(self.pipe_v0, "video0")
        self.pipe_v0.set_state(Gst.State.PLAYING)
        self.all_pipes.append(self.pipe_v0)

        # video 1
        self.pipe_v1, vsink1 = build_video_pipeline(PORT_VIDEO_S1)
        self._inject_gtksink(vsink1, self.slot1)
        self._attach_bus(self.pipe_v1, "video1")
        self.pipe_v1.set_state(Gst.State.PLAYING)
        self.all_pipes.append(self.pipe_v1)

        # audio in
        self.pipe_audio_in = build_audio_in_pipeline()
        self._attach_bus(self.pipe_audio_in, "audio-in")
        self.pipe_audio_in.set_state(Gst.State.PLAYING)
        self.all_pipes.append(self.pipe_audio_in)

        # audio out (optional)
        if self.enable_mic:
            self._start_mic()

        # check status after 3 seconds
        GLib.timeout_add(3000, self._poll_status)
        return False  # idle_add oneshot

    def _inject_gtksink(self, vsink, slot):
        """Pull gtksink's native widget and embed it in the slot."""
        gst_widget = vsink.get_property("widget")
        if gst_widget is None:
            print("[WARN] gtksink returned no widget")
            return
        parent = slot["placeholder"].get_parent()
        if parent:
            parent.remove(slot["placeholder"])
            parent.pack_start(gst_widget, True, True, 0)
            gst_widget.set_size_request(630, 360)
            gst_widget.show()

    def _start_mic(self):
        self.pipe_audio_out = build_audio_out_pipeline(self.board_ip)
        self._attach_bus(self.pipe_audio_out, "audio-out")
        ret = self.pipe_audio_out.set_state(Gst.State.PLAYING)
        if ret == Gst.StateChangeReturn.FAILURE:
            print("[WARN] Mic pipeline failed — disabling")
            self.pipe_audio_out = None
            GLib.idle_add(lambda: self._set_mic_label(False) or False)
        else:
            self.all_pipes.append(self.pipe_audio_out)
            print(f"[INFO] Mic active → board {self.board_ip}:{PORT_AUDIO_OUT}")

    def _stop_mic(self):
        if self.pipe_audio_out:
            self.pipe_audio_out.set_state(Gst.State.NULL)
            if self.pipe_audio_out in self.all_pipes:
                self.all_pipes.remove(self.pipe_audio_out)
            self.pipe_audio_out = None
            print("[INFO] Mic stopped")

    # ── bus messages ──────────────────────────────────────────
    def _attach_bus(self, pipe, name):
        bus = pipe.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self._on_bus_msg, name)

    def _on_bus_msg(self, bus, msg, name):
        t = msg.type
        if t == Gst.MessageType.ERROR:
            err, _ = msg.parse_error()
            print(f"[ERROR] {name}: {err.message}")
            GLib.idle_add(lambda: self._set_status("error") or False)
            GLib.timeout_add(4000, lambda: self._reconnect(name))
        elif t == Gst.MessageType.STATE_CHANGED:
            if msg.src in (self.pipe_v0, self.pipe_v1):
                _, new, _ = msg.parse_state_changed()
                if new == Gst.State.PLAYING:
                    GLib.idle_add(lambda: self._set_status("live") or False)

    def _reconnect(self, name):
        print(f"[INFO] Reconnecting {name}…")
        pipe = {
            "video0":   self.pipe_v0,
            "video1":   self.pipe_v1,
            "audio-in": self.pipe_audio_in,
        }.get(name)
        if pipe:
            pipe.set_state(Gst.State.NULL)
            pipe.set_state(Gst.State.PLAYING)
        return False

    # ── status helpers ────────────────────────────────────────
    def _poll_status(self):
        if self.pipe_v0:
            _, state, _ = self.pipe_v0.get_state(0)
            if state == Gst.State.PLAYING:
                self._set_status("live")
            else:
                self._set_status("connecting")
        return False

    def _set_status(self, kind):
        ctx = self.status_lbl.get_style_context()
        ctx.remove_class("status-live")
        ctx.remove_class("status-connecting")
        ctx.remove_class("status-error")
        if kind == "live":
            ctx.add_class("status-live")
            self.status_lbl.set_text("● live")
        elif kind == "error":
            ctx.add_class("status-error")
            self.status_lbl.set_text("● error — retrying")
        else:
            ctx.add_class("status-connecting")
            self.status_lbl.set_text("● connecting")

    def _set_mic_label(self, active):
        self.mic_btn.set_active(active)
        self.mic_btn.set_label("● Mic ON" if active else "○ Mic OFF")

    # ── controls ──────────────────────────────────────────────
    def _on_mic_toggled(self, btn):
        if btn.get_active():
            self._set_mic_label(True)
            self._start_mic()
        else:
            self._set_mic_label(False)
            self._stop_mic()

    def _on_quit(self, *_):
        print("\n[INFO] Shutting down…")
        for p in self.all_pipes:
            p.set_state(Gst.State.NULL)
        Gtk.main_quit()
        return True


# ─── entry point ──────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Hailo Dual Sensor Receiver — run on host machine",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 host_receiver.py
  python3 host_receiver.py --board-ip 10.0.0.1
  python3 host_receiver.py --board-ip 10.0.0.1 --no-mic
        """
    )
    parser.add_argument(
        "--board-ip", default="10.0.0.1",
        help="IP of the Hailo board (default: 10.0.0.1)"
    )
    parser.add_argument(
        "--no-mic", action="store_true",
        help="Disable host mic → board talkback"
    )
    args = parser.parse_args()

    Gst.init(None)

    app = ReceiverApp(board_ip=args.board_ip, enable_mic=not args.no_mic)
    GLib.idle_add(app.start_pipelines)

    signal.signal(signal.SIGINT, lambda *_: GLib.idle_add(Gtk.main_quit))

    print("=" * 50)
    print("  Hailo Dual Sensor Receiver")
    print("=" * 50)
    print(f"  Board IP   : {args.board_ip}")
    print(f"  Video      : :5000 (sensor0)  :5100 (sensor1)")
    print(f"  Audio in   : :5200 board mic -> host speakers")
    print(f"  Audio out  : :5201 host mic -> board speaker")
    print(f"  Mic        : {'enabled' if not args.no_mic else 'disabled (--no-mic)'}")
    print("  Close window or Ctrl+C to quit")
    print("=" * 50)

    Gtk.main()


if __name__ == "__main__":
    main()
