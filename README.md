# JARVIS

> AI-powered terminal assistant for Linux. Written in pure C. Zero bloat.

![Platform](https://img.shields.io/badge/platform-Linux-blue)
![Language](https://img.shields.io/badge/language-C99-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)
![Version](https://img.shields.io/badge/version-2.3.0-cyan)

---

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/Tehns/Jarvis/main/install.sh | bash
```

Get a free Gemini API key at [aistudio.google.com](https://aistudio.google.com), add it to `~/.config/jarvis/config` and you're done.

---

## What it does

### Explain any error — just pipe it

```bash
make 2>&1 | jarvis explain
gcc foo.c 2>&1 | jarvis explain
cargo build 2>&1 | jarvis explain
npm install 2>&1 | jarvis explain
systemctl start nginx 2>&1 | jarvis explain
```

Output:
```
── Jarvis is reading the error ────────────────

  ✗ The linker cannot find -lssl because OpenSSL is not installed.
  ✓ sudo pacman -S openssl
  i Your Makefile links against OpenSSL but the library isn't present on this system.
```

### Watch a command — auto-explain on failure

```bash
jarvis watch make
jarvis watch cargo build
jarvis watch gcc foo.c
```

Runs the command live. If it fails, Jarvis automatically explains what went wrong and how to fix it. No extra steps.

### Suggest aliases from your history

```bash
jarvis alias
```

Sends your recent history to Gemini and gets back ready-to-paste alias suggestions based on what you actually type.

```
-- Alias suggestions ----------------------
   Sending your history to Gemini...

  alias gst='git status'
  alias gp='git push'
  alias mkinstall='sudo make install'
  alias ..='cd ..'

  Paste into ~/.bashrc, then: source ~/.bashrc
```

### Ask for any command

```bash
jarvis "find files modified in the last 24 hours"
# Jarvis suggests: find . -mtime -1 -type f
# Run it? (y/n) >
```

### Full chat mode

```bash
jarvis talk
```

Persistent conversation context. Type `back` to return to command mode.

### Weather & system info

```bash
jarvis weather
jarvis weather London
jarvis sysinfo
```

### Update

```bash
jarvis update
```

---

## Interactive mode

```
jarvis > find large log files
jarvis > watch make
jarvis > alias
jarvis > weather Kyiv
jarvis > talk
jarvis > sysinfo
jarvis > help
jarvis > q
```

---

## One-shot from terminal

```bash
jarvis "kill process on port 8080"
jarvis watch "cargo build --release"
jarvis explain < error.log
```

---

## Config

Location: `~/.config/jarvis/config`

```ini
api_key=YOUR_GEMINI_API_KEY
city=Kharkiv
history_path=/home/you/.bash_history
```

| Key | Description | Default |
|---|---|---|
| `api_key` | Google Gemini API key (free) | *(required)* |
| `city` | Default city for weather | `Kharkiv` |
| `history_path` | Shell history file | *(auto-detected)* |

Supports **bash, zsh, and fish** — history is auto-detected.

---

## Build from source

```bash
git clone https://github.com/Tehns/Jarvis.git
cd Jarvis
make
sudo make install
```

**Dependencies:** `gcc`, `libcurl`

```bash
sudo pacman -S curl gcc    # Arch
sudo apt install gcc libcurl4-openssl-dev    # Debian/Ubuntu
sudo dnf install gcc libcurl-devel           # Fedora
```

---

## Why C?

- Single binary, ~100KB
- No runtime, no interpreter, no dependencies beyond libcurl
- Works on any Linux — x86, ARM, i686
- Starts instantly

---

## License

MIT
