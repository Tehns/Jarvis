# Contributing to Jarvis

Thanks for wanting to contribute! Here's how to do it cleanly.

## Getting started

```bash
git clone https://github.com/Tehns/Jarvis.git
cd Jarvis
make
./jarvis
```

**Dependencies:** `gcc`, `libcurl`, `make`

## Project structure

Everything lives in `main.c` - that's intentional. Jarvis is a single-file project. Keep it that way.

## How to contribute

1. **Fork** the repo
2. **Create a branch** - `git checkout -b feature/your-feature`
3. **Make your changes** in `main.c`
4. **Test it** - `make && ./jarvis`
5. **Open a pull request** with a clear description of what you changed and why

## Guidelines

- Keep it C99, no C++ 
- No new dependencies beyond `libcurl`
- No new files - everything stays in `main.c`
- New commands follow the existing pattern: `cmd_yourcommand()` function + wired into `main()`
- Test on at least one Linux distro before submitting

## Good first contributions

- Fix a bug from the [issues](https://github.com/Tehns/Jarvis/issues) list
- Add support for a new shell history format
- Improve error messages
- Add a new `sysinfo` metric

## Reporting bugs

Open an issue with:
- What you ran
- What you expected
- What actually happened
- Your distro and gcc version (`gcc --version`)

## Feature requests

Open an issue with the `enhancement` label. Describe the use case, not just the feature.
