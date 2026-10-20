# ps1-writer

A text editor for the original PlayStation (PS1 / PSone), made for writing long documents such as a thesis. Typing is done on an ordinary **USB keyboard**, and documents are stored on an **SD card**. A Raspberry Pi Pico 2 on **controller port 2** handles both.

An optional **AI assistant** runs on a PC connected to the Pico by USB. It can read the document, answer questions about it, and write text that goes straight into it.

```
USB keyboard ──> Pico 2 ──> controller port 2 ──> PS1: editor, 80 columns
SD card      <──>  (UTF-8 .md / .txt files you can open on any PC)
PC (USB)     <──>  pc/ai_bridge.py ──> Claude, a local model, or any OpenAI-compatible API
```

![editor](docs/editor.png)

## Features

- 80-column text screen (640×240, 8×8 font). The number of rows is adjustable (default 28, F11/F12) because many TVs and video converters cut off the top and bottom lines.
- USB keyboard, US layout, with key repeat.
- Documents up to 1 MB, kept in PS1 memory as paragraphs with soft word wrap. Text is Latin-1 (ISO-8859-1) on the PS1 and UTF-8 on the card.
- Saving sends **only what changed**. After a sentence is typed into a 300 KB thesis, a few hundred bytes cross the bus. Each save is checked with a size and CRC-32 test before the old file is replaced.
- You can **keep typing while the card is being written**.
- Autosave runs after 3 s without typing, at most once a minute. The last 5 versions of every document are kept in `/ps1writer/backup/`.
- Open, new and find dialogs. Markdown-friendly: lines starting with `#` are shown as headings.
- **AI chat** (F2): the PC sees the saved document and where the cursor is. F4 inserts its answer at the cursor, and F5 replaces the whole document with it.

![AI chat](docs/ai_chat.png)

## Contents

| Path | What it is |
|---|---|
| `ps1/` | PS1 program (C, PSn00bSDK): editor, document and save logic, bus client, screen |
| `pico/` | Pico 2 firmware. Core 0: the PS1 bus (PIO). Core 1: USB keyboard (PIO-USB), the Pico's own USB (log + AI link), SD card, file and AI requests |
| `common/` | protocol definition (`pw_protocol.h`) and Latin-1 ⇄ UTF-8 conversion, shared by both sides |
| `pc/ai_bridge.py` | the PC side of the AI link |
| `kbtest/` | keyboard-only test firmware (same wiring, no PS1 or SD card needed) |
| `tests/` | PC tests: the real editor talks to the real Pico file server, AI link and bus logic; the real bridge talks to the real AI link code |
| `tools/` | `make_font.py` (font header), `preview.py` (draws screens dumped by the tests), `make_techpdf.py` (builds the technical reference) |
| `docs/` | screenshots and [`technical-reference.pdf`](docs/technical-reference.pdf): the bus protocol, every request, the save procedure and a porting guide for the Pico firmware |

### Downloads

Ready-built files are on the **Releases** page, so you don't need any toolchain to try it:

- `ps1_writer.uf2`: Pico 2 firmware
- `kbtest.uf2`: keyboard test firmware
- `ps1writer_250k`, `ps1writer_500k`, `ps1writer_1mhz` (`.bin` + `.cue`): PS1 disc images at three bus speeds

### Getting the source

The Pico firmware uses two libraries as git submodules, so clone with `--recursive`:

```
git clone --recursive <repository URL>
```

In an existing clone, `git submodule update --init` fetches them.

## Requirements

- A PS1 or PSone that can boot burned CD-Rs (modchip or similar)
- Raspberry Pi Pico 2 (RP2350)
- An SD card (bare card or SPI module), FAT32 or exFAT
- A USB keyboard and a USB-A socket (female) with 4 wires
- A way into controller port 2, ideally a cut controller extension cable
- For the AI chat: a PC with Python 3 and `pyserial`, plus an API key or a local model server

## Wiring

**Controller port 2 → Pico 2.** Seen from the front of the console socket, the pins run 9 8 7 | 6 5 4 | 3 2 1. The cable plug is the mirror image, so wire by signal name and check with a multimeter: about 7.6 V on pin 3, about 3.5 V on pin 5, and pin 4 is GND.

| Port pin | Signal | Pico GPIO | Pico pin |
|---|---|---|---|
| 1 | DATA | GP5 | 7 |
| 2 | CMD | GP2 | 4 |
| 3 | 7.6 V | **not connected** | – |
| 4 | GND | GND | 8 |
| 5 | 3.5 V | **not connected** | – |
| 6 | ATT (select) | GP4 | 6 |
| 7 | CLK | GP3 | 5 |
| 8 | unused | – | – |
| 9 | ACK | GP6 | 9 |

**USB keyboard socket → Pico 2.** The keyboard runs on two ordinary GPIOs (PIO-USB, low/full speed). The Pico's own USB port stays free for power, the log and the AI link.

| USB-A socket pin | Signal | Pico | Pico pin |
|---|---|---|---|
| 1 (usually red) | VBUS 5 V | VBUS | 40 |
| 2 (usually white) | D− | GP13 | 17 |
| 3 (usually green) | D+ | GP12 | 16 |
| 4 (usually black) | GND | GND | 18 |

- VBUS carries the 5 V from the Pico's own USB cable, so power the Pico from a PC or a USB charger. A 47–100 µF capacitor across the socket's 5 V and GND helps if the Pico resets when the keyboard is plugged in.
- Optional: 22 Ω resistors in series with D+ and D−, and 15 kΩ resistors from D+ and from D− to GND (the standard USB host pull-downs). Try the pull-downs if the keyboard is not detected reliably; the RP2350's internal pull-downs are affected by its erratum E9.
- Keep these two wires short and twisted together.

**SD card → Pico 2** (SPI). Bare full-size card pins in brackets.

| SD signal | Pico GPIO | Pico pin |
|---|---|---|
| CS (1) | GP17 | 22 |
| MOSI (2) | GP19 | 25 |
| GND (3 and 6) | GND | 23, 38 |
| 3.3 V (4) | 3V3(OUT) | 36 |
| SCK (5) | GP18 | 24 |
| MISO (7) | GP16 | 21 |

The card clock is 1 MHz, which works with long breadboard wires. With short wires, raise `baud_rate` in `pico/hw_config.c` to 4 MHz: opening and saving big documents gets several times faster.

**Safety**

- Never connect pin 3 (7.6 V) or pin 5 (3.5 V) of the controller port to the Pico.
- Power the Pico before the console, and switch the console off first when you're done.
- Keep memory card slot 2 empty, because it shares the select line with controller port 2.

## 1. Flash the Pico

Hold BOOTSEL, plug the Pico 2 into USB and copy `ps1_writer.uf2` (from the release) to the drive that appears.

The Pico's own USB port now shows up as **two serial ports**: "ps1-writer log" and "ps1-writer AI" (on Linux usually `/dev/ttyACM0` and `/dev/ttyACM1`, on Windows two COM ports). The log port (any baud rate) prints a status line every 5 seconds, for example `status: SD ok, keyboard connected, AI bridge not running`, plus every open and save. The LED blinks slowly while the firmware runs and flickers during file requests; if it stays dark, the firmware has stopped.

Building it yourself (Pico SDK 2.2 with `PICO_SDK_PATH` set, submodules fetched):

```
cd pico
cmake -B build
cmake --build build
```

The output is `build/ps1_writer.uf2`. The submodules are pinned to tested versions; Pico-PIO-USB in particular must be the one TinyUSB 0.18 (Pico SDK 2.2) expects, so don't update it on its own.

### Keyboard test

`kbtest.uf2` (from the release) tests only the keyboard socket: no PS1 or SD card needed. Flash it, open the Pico's serial port and plug in the keyboard. It reports:

- when something is electrically detected on the socket (wiring OK)
- the USB device (VID:PID) and whether it is a keyboard
- every key report in hex and the character it becomes
- each typed line on Enter

If a device is detected but never becomes a keyboard, it suggests what to check. The source is in `kbtest/`, and it builds like the firmware (it uses `pico/lib/Pico-PIO-USB`).

### Known keyboard issues

- **Plug the keyboard in after the Pico has started.** With the keyboard already connected while the Pico powers up, the main firmware does not detect it.
- **Not every keyboard works.** In testing, one wired mechanical keyboard did not work, while a wireless keyboard with a USB receiver did. If yours does not, `kbtest` shows how far it gets.

## 2. Burn the PS1 disc

The three disc images in the release are identical except for the controller-bus clock:

| Disc | Bus clock | Opening a 300 KB document |
|---|---|---|
| `ps1writer_250k` | 250 kHz (stock) | ~15 s, start here |
| `ps1writer_500k` | 500 kHz | ~9 s |
| `ps1writer_1mhz` | 1 MHz | ~6 s, if the wiring keeps up |

The times above are for the bus only. With the SD card at 1 MHz, the Pico also needs a few seconds to convert the file.

Burn the `.cue` to a CD-R at the slowest speed and boot it.

Building it yourself:

1. Download PSn00bSDK 0.24 from its GitHub releases (it includes the `mipsel-none-elf-gcc` toolchain and `mkpsxiso`), plus CMake 3.21+ and Ninja.
2. Add the SDK's `bin` folder to `PATH` and set `PSN00BSDK_LIBS` to its `lib/libpsn00b` folder.
3. In `ps1/`, run `cmake --preset default` then `cmake --build build`. Add `-DSIO_BAUD_RELOAD=0x44` to the first command for 500 kHz.
4. The image is `build/ps1writer.bin` + `build/ps1writer.cue`.

## Using it

Power the Pico, plug in the keyboard, then switch on the console with the disc. The editor connects to the Pico, waits for the SD card and reopens the last document. On the first start it shows the files in `/ps1writer/`, or asks for a name for a new document (default `thesis.md`).

| Key | Action |
|---|---|
| Ctrl+S | save (also automatic) |
| Ctrl+O / Ctrl+N | open / new document (the current one is saved first) |
| Ctrl+F, F3 or Ctrl+G | find, find next (ignores case) |
| arrows, Home/End, PgUp/PgDn | move; with Ctrl: by word / paragraph / document start-end |
| Ctrl+Backspace | delete the word to the left |
| Insert | insert / overwrite mode |
| Tab | 4 spaces |
| F1 | help |
| F2 | AI chat |
| F4 | insert the last AI answer at the cursor |
| F11 / F12 | fewer / more screen rows |

The status bar shows the file name (`*` = unsaved changes), paragraph and column, `AI` when the PC bridge is connected, the word count, the mode, and warnings: `NO KEYBOARD`, `NO SD CARD`, `NO PICO`.

**Screen size.** If the first or last line is cut off on your TV or video converter, press F11 until everything fits (20–30 rows; the text stays centred). The setting is saved on the SD card.

**On the SD card**

```
/ps1writer/            your documents: UTF-8 text, edit them on a PC too
/ps1writer/backup/     thesis.md.1 (newest) ... thesis.md.5 (oldest)
/ps1writer/.work/      the Pico's working files and settings, don't touch
```

Documents written on a PC open fine. Characters that Latin-1 doesn't have are replaced: typographic quotes become " and ', dashes become -, … becomes ..., € becomes EUR, a few accented letters become their plain form (ş → s), and anything else (emoji, for example) becomes `?`. The replacement is written back on the next save. Windows line ends are converted to plain ones. File names with such characters still open; they are shown with the same replacements.

## AI chat

The Pico passes questions from the PlayStation to `pc/ai_bridge.py` over its second USB serial port. The bridge sends them to a language model together with the document and streams the answer back.

```
pip install pyserial

# Claude with an API key from console.anthropic.com (billed per use)
export ANTHROPIC_API_KEY=...
python3 pc/ai_bridge.py --backend anthropic

# Claude through Claude Code, with the account it is logged in to
# (a Pro / Max subscription: no API key, counts against the plan's limits)
python3 pc/ai_bridge.py --backend claude-code

# a local model with Ollama (or LM Studio, llama.cpp server, OpenAI itself...)
python3 pc/ai_bridge.py --backend openai --url http://localhost:11434/v1 --model llama3.1

# no model at all: checks the link
python3 pc/ai_bridge.py --backend echo
```

A Claude subscription does not include API access, which is why there are two Claude options. For `claude-code`, install [Claude Code](https://docs.claude.com/en/docs/claude-code/overview) and log in once (`claude` in a terminal). The bridge runs `claude -p` with no tools and no MCP servers, so it only writes text. It hides `ANTHROPIC_API_KEY` from Claude Code so the subscription is used, not the key (`--keep-api-key` changes that). `--model` picks a model, and `--claude-bin` points at the program if it is not on the PATH.

The bridge finds the "ps1-writer AI" port by itself and reconnects if the Pico is unplugged. `--port` picks a port by hand, and `--list` shows all ports.

**On the PlayStation:**

1. Press F2 and type a question, for example "continue this paragraph" or "check the grammar of section 2.1". Press Enter.
2. The document is saved first. The bridge then sends the saved text, with a `<<CURSOR>>` marker where the cursor is, plus the earlier questions and answers.
3. The answer appears in the chat panel as it arrives.
4. F4 inserts the last answer at the cursor. F5, pressed twice, replaces the whole document with it; the old version stays in `backup/`. Esc cancels a question; Up recalls the last one.
5. Typing `/new` starts a fresh conversation.

The model is told that the editor shows plain text and Latin-1 only, and to answer writing requests with just the text to insert.

**Good to know**

- **Privacy:** with `anthropic`, `claude-code` or a cloud OpenAI-compatible service, the whole document goes to that provider with every question. A local model (Ollama, LM Studio) keeps it on your PC.
- **Cost:** the document is sent with every question, so long documents use many tokens (or much of a subscription's usage limit). `--max-doc-chars` (default 200000) limits how much is sent; beyond that, the part around the cursor is used.
- **Answer length:** answers are cut at 64 KB on the Pico. `--max-tokens` (default 2000) sets the model's limit.

## How saving works

On opening, the Pico converts the document to Latin-1 and keeps that working copy. The PS1 remembers which part of the working copy each paragraph came from.

To save, the PS1 sends a list of pieces: *keep bytes x–y of the working copy* for unchanged paragraphs, and the text itself for changed ones. The Pico builds the new working copy and the new UTF-8 file in one pass, then compares the size and CRC-32 with what the PS1 computed. Only if they match does it move the old document to `backup/` and the new one into place.

If anything doesn't match, the PS1 sends the whole document instead. This happens, for example, after the Pico has restarted.

The bus itself is protected too:

- Every request and response carries a CRC-16 and a sequence number, and lost or damaged transfers are repeated.
- The tests flip random bits on the bus and check that every save still arrives intact.
- Keyboard events are removed from the Pico's queue only after the PS1 has read them completely.

## Limitations

- No undo yet. The backups are the safety net.
- No block selection, copy or paste yet.
- No clock on the Pico, so backups are "the last 5 saves", not dated.
- Latin-1 only; other characters are replaced when a document is opened (see above).
- US keyboard layout only.

## Tests

```
cd tests
make test
```

The tests compile the PS1 editor, the Pico's bus logic, file server and AI link for the PC. A folder stands in for the SD card and a pretend PC for the AI bridge. They cover:

- opening and converting, typing, saving and backups
- random editing with a save after each round, checking that the card always equals the document
- a 250 KB document
- a damaged working copy and a Pico restart
- accented and non-Latin-1 file names, wrapping and cursor movement, find, autosave
- typing while a slow card is being written
- screen rows and their saved setting
- the AI chat: question, document and cursor sent, streamed answer, insert, replace, bridge gone
- bit errors on the bus, and the keyboard

`test_bridge.py` then runs the real `pc/ai_bridge.py` against the Pico's AI link code over a pseudo-terminal, with the echo backend. It also checks the Anthropic and OpenAI stream parsers against a local server and the Claude Code backend against a stand-in `claude` program. It needs no extra Python packages.

`tools/preview.py` draws the screens the tests leave behind (`tests/screen_*.bin`).

## Protocol

The Pico answers on the controller bus with device address `0x22`, so it never clashes with a controller (`0x01`) or memory card (`0x81`). Three transactions exist:

- **KEYS**: polled every frame; up to 16 events of 3 bytes.
- **REQUEST**: up to 2 KB plus a CRC.
- **RESPONSE**: fetches the answer.

File and AI work happens on the Pico's second core, so the bus never waits for the card or the PC. See `common/pw_protocol.h` for the PS1 side and `pico/ai.h` for the PC side.

## Credits

The libraries in `pico/lib/` are git submodules and keep their own licences.


- [PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK) – PS1 SDK
- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) and [TinyUSB](https://github.com/hathach/tinyusb)
- [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) – USB host on GPIO pins
- [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico) – SD card / FatFs library
- [font8x8](https://github.com/dhepper/font8x8) – public-domain 8×8 font
- [psx-spx](https://psx-spx.consoledev.net/) – PS1 hardware documentation
