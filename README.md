# MCRoute

Small Minecraft TCP proxy with a terminal UI.

MCRoute listens on port `25565`, forwards connections to a selected preset, and
can customize the server-list title, MOTD, subtitle, and player counts.

## Build and run

```sh
cmake -S . -B build
cmake --build build -j
./build/MCRoute
```

TUI keys: arrows select, `Enter` activates, `A` adds, `I` edits server info,
`L` opens the debug log, and `Q` quits.

Preset format:

```text
name|host|port|motd|subtitle|online|max|title
```

Example:

```text
survival|192.168.1.20|25565|My Server|Welcome|3|20|1.21.8
```

MCRoute is a network proxy, not a Minecraft server. The target controls
authentication, gameplay, and access policy. Use it only with servers you own
or are authorized to access; it is not intended to evade blacklists ;).