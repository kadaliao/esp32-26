# deck-usage

A Claude Code mod with no UI. Claude Code already receives your subscription usage windows
(5-hour and 7-day, percent used and reset time) with every API response; whenever a window moves
by a whole point the engine pushes a `session.measure` event, and this mod writes the windows to
`~/Library/Caches/deck/claude-limits.json`. The desk panel (`../host/apps/quota.py`) reads that
file, so it never has to run `claude -p /usage` or send any request of its own.

```json
{"v":1,"at":1793606000000,"limits":[{"kind":"five_hour","used":11,"resets_at":"2026-11-02T10:59:59Z"},
                                    {"kind":"seven_day","used":17,"resets_at":"2026-11-08T14:59:59Z"}]}
```

## Install (local, read straight from this folder)

```bash
claude plugin marketplace add /path/to/esp32-26/deck/claude-mod
claude plugin install deck-usage@esp32-deck
```

Edits here reach running sessions after `/reload-plugins`. Uninstall: `claude plugin uninstall deck-usage@esp32-deck`.
