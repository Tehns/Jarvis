# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 2.x     | ✓         |
| 1.x     | ✗         |

## Reporting a vulnerability

**Do not open a public issue for security vulnerabilities.**

If you find a security issue (e.g. shell injection, credential leak, buffer overflow), please report it privately:

1. Go to the [Security tab](https://github.com/Tehns/Jarvis/security) on GitHub
2. Click **Report a vulnerability**
3. Describe the issue, steps to reproduce, and potential impact

You'll get a response within 48 hours. If the vulnerability is confirmed, a fix will be released as soon as possible and you'll be credited in the release notes.

## Security notes

- Your Gemini API key is stored in `~/.config/jarvis/config` - keep this file private (`chmod 600`)
- Jarvis sends your shell history context to the Gemini API - avoid using Jarvis in environments with sensitive command history
- `jarvis watch` uses `fork`+`execvp`, not `system()` - no shell injection risk from user input
