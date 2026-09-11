"""Read the repeater's boot-stage marker straight out of flash.

On ESP32-C5 the USB-Serial/JTAG console is often unusable for boot debugging:
a reset issued over the CDC control lines drops the chip into download mode,
and a wedged CPU jams the endpoint. So the firmware records how far it got in
NVS instead, and this script reads it back.

Usage (chip must be in ROM download mode -- hold BOOT, tap RESET, release BOOT):

    python tools/read_boot_stage.py [PORT]

Prints the boot stage and the PHY guard flag.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

# Default single-app-large layout: nvs at 0x9000, 24 KB.
NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000

STAGES = {
    0: "no marker (NVS erased, or app never reached esp_netif_init)",
    1: "app_main entered",
    2: "NVS + netif + event loop ready",
    3: "wifi driver initialised (init_wifi returned)",
    4: "esp_wifi_start() returned",
    5: "PHY config applied (band mode / protocols / bandwidth)",
    6: "SoftAP started (APSTA mode)",
    7: "esp_wifi_connect() issued",
    8: "HTTP server started",
    9: "boot complete, all tasks running",
}


def read_nvs(port):
    out = Path(tempfile.gettempdir()) / "repeater_nvs.bin"
    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32c5", "-p", port,
        # default-reset puts the chip into ROM download mode over the CDC lines,
        # which is what we need to read flash. --after no-reset leaves it there
        # so several reads in a row work without re-entering download mode.
        "--before", "default-reset", "--after", "no-reset",
        "read-flash", hex(NVS_OFFSET), hex(NVS_SIZE), str(out),
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(
            "esptool failed. If it reported 'Write timeout' the app is most likely\n"
            "running and holding the USB-Serial/JTAG endpoint -- retry, and if it\n"
            "keeps failing hold BOOT, tap RESET, release BOOT, then run again.\n\n"
            + (res.stdout or "") + (res.stderr or "")
        )
    return out.read_bytes()


PAGE_SIZE = 4096
HEADER_SIZE = 32
BITMAP_SIZE = 32
ENTRY_SIZE = 32
ENTRIES_PER_PAGE = 126

ENTRY_WRITTEN = 0b10
NVS_TYPE_U8 = 0x01


def _entry_state(bitmap, index):
    """Two bits per entry, little-endian within each byte."""
    byte = bitmap[index // 4]
    return (byte >> ((index % 4) * 2)) & 0b11


def find_u8(blob, key):
    """Return the current value of a u8 NVS entry, or None.

    Walks the NVS pages properly instead of grepping: only entries the state
    bitmap marks as WRITTEN count, and pages are ordered by their sequence
    number so a compacted page cannot hand back a stale value.
    """
    key_bytes = key.encode()
    best = None          # (page_seqno, entry_index, value)

    for page_start in range(0, len(blob) - PAGE_SIZE + 1, PAGE_SIZE):
        page = blob[page_start:page_start + PAGE_SIZE]
        page_state = int.from_bytes(page[0:4], "little")
        if page_state == 0xFFFFFFFF:      # uninitialised page
            continue
        seqno = int.from_bytes(page[4:8], "little")
        bitmap = page[HEADER_SIZE:HEADER_SIZE + BITMAP_SIZE]
        entries = page[HEADER_SIZE + BITMAP_SIZE:]

        for i in range(ENTRIES_PER_PAGE):
            if _entry_state(bitmap, i) != ENTRY_WRITTEN:
                continue
            e = entries[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
            if len(e) < ENTRY_SIZE or e[1] != NVS_TYPE_U8:
                continue
            if e[8:24].split(b"\x00")[0] != key_bytes:
                continue
            cand = (seqno, i, e[24])
            if best is None or cand[:2] > best[:2]:
                best = cand

    return None if best is None else best[2]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    blob = read_nvs(port)

    stage = find_u8(blob, "bootstg")
    guard = find_u8(blob, "phy_try")

    if stage is None:
        print("boot stage : <key not found> -- firmware never wrote a marker")
    else:
        print("boot stage : {} -- {}".format(stage, STAGES.get(stage, "unknown")))

    if guard is None:
        print("phy guard  : <key not found>")
    elif guard:
        print("phy guard  : ARMED -- radio config did not finish; next boot "
              "comes up in PHY SAFE MODE")
    else:
        print("phy guard  : clear -- radio config completed successfully")


if __name__ == "__main__":
    main()
